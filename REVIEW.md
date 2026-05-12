# Code Review – ESP32 Wi-Fi RSSI/CSI Sensor

Date: 2026-05-12
Reviewed against: `task1.md`
Scope: complete project (firmware + Python collector + docs)
Reviewer mode: read-only, no code changes

---

## 1. Overall verdict

The implementation covers the full breadth of `task1.md`. All deliverables exist in
the expected files, the boot sequence matches the spec, and the binary protocol
is documented end-to-end. However, the project ships with **three classes of
defects that will break it on first run**:

1. Protocol size mismatch between firmware (C structs), README, and Python collector → `assert` failures in `collector.py` and wrong byte offsets in README.
2. A ring buffer that is **not safe for two concurrent producers**, despite being fed by two Wi-Fi callbacks that can fire independently.
3. A 512-byte stack buffer inside a Wi-Fi driver callback (CSI) that is dangerously close to typical Wi-Fi task stack limits.

None of these would be caught by a clean `idf.py build`; they all show up at run-time. They must be fixed before the firmware can be considered acceptable.

Severity legend: 🔴 blocker · 🟠 major · 🟡 minor · 🟢 nit / suggestion

---

## 2. Requirements traceability

| # | `task1.md` requirement | Status | Notes |
|---|---|---|---|
| 1 | Use ESP-IDF | ✅ | `CMakeLists.txt:2` includes `project.cmake`. |
| 2 | Fixed 2.4 GHz channel | ✅ | `wifi_sensor.c:176` `esp_wifi_set_channel(...)`. |
| 3 | Promiscuous mode + frame filter flags | ✅ | `wifi_sensor.c:78–93` honors `CAPTURE_*`. |
| 4 | Binary RSSI records w/ required fields | ⚠️ | All fields present (`record_protocol.h:29–42`); **size/offset documentation wrong** (see §3.1). |
| 5 | CSI capture, no heavy work in callback | ⚠️ | Structure correct, but 512-byte stack alloc in callback is risky (§3.3). |
| 6 | Bounded queue + drop counter + status records | ⚠️ | Implemented (`ring_buffer.c`, `serial_writer.c:72-89`); not multi-producer safe (§3.2). |
| 7 | Configurable binary-safe UART, default 921600, no log pollution | ✅ | `serial_writer.c:40-63`. |
| 8 | Minimal config via compile-time flags | ✅ | `sensor_config.h`. Menuconfig is documented as future, acceptable per spec. |
| Init order (NVS → netif → event → wifi → promisc → CSI) | ✅ | `main.c:17-45` + `wifi_sensor.c:162-201`. |
| Callback safety (no malloc / no UART / no log / non-blocking) | ✅ for promisc, ⚠️ for CSI | See §3.3. |
| Acceptance criteria 1–11 | ⚠️ | #10 (collector decodes RSSI/STATUS) will fail at import time due to size assertions (§3.1). |
| Deliverable: README with full protocol description | ⚠️ | Present but the offset/size tables disagree with the C structs (§3.1). |

---

## 3. Bugs and risks

### 3.1 🔴 Binary protocol size mismatch (firmware vs README vs Python)

The packed C structs in `record_protocol.h` produce these sizes:

| Struct | Computed size (packed) | Comment claims | README claims | Python `struct.calcsize` |
|---|---|---|---|---|
| `rssi_record_t` | **26** | `// total: 24 bytes` (`record_protocol.h:42`) | `RSSI Record (24 bytes)` (`README.md:89`) | **26** (`<HBBIIBbBB6sHH`) |
| `csi_record_header_t` | **24** | `// total: 22 bytes` (`record_protocol.h:56`) | `22-byte header` (`README.md:110`) | **24** (`<HBBIIBb6sHH`) |
| `status_record_t` | **27** | `// total: 23 bytes` (`record_protocol.h:70`) | `23 bytes` (`README.md:126`) | **27** (`<HBBIIIIIbH`) |

Field-by-field accounting of `rssi_record_t` (packed, little-endian):
`magic(2) + version(1) + record_type(1) + seq(4) + timestamp_us(4) + channel(1) + rssi(1) + frame_type(1) + frame_subtype(1) + mac(6) + payload_len(2) + crc16(2) = 26 bytes` — not 24.

Consequences:
1. **`tools/collector.py:59-61` will abort at import time** with one of:
   `AssertionError: RSSI size mismatch: 26` (or 24/27 for CSI/STATUS).
   This means the very first `python tools/collector.py …` invocation listed in the README (and in §3 of `task1.md` Preferred First Test Scenario) cannot succeed.
2. The README protocol tables (`README.md:89-139`) have wrong offsets for `payload_len`, `crc16`, and `channel`. E.g. for RSSI, `crc16` is listed at offset 23 (impossible — it is `uint16` and offset 22 is taken by `payload_len`); it is actually at offset 24.
3. Anyone implementing a third-party reader from the README alone will be off by 2 bytes per record and immediately desync.

The actual struct *layout* on the wire is correct and self-consistent; only the
*documented sizes* are wrong. Fix by updating the comments in
`record_protocol.h`, the README tables, and the three `assert` statements in
`tools/collector.py` to 26 / 24 / 27.

### 3.2 🔴 Ring buffer is not safe between two producers

`ring_buffer.h:12-15` (the comment) claims "atomically using a FreeRTOS spinlock so it can be called from a Wi-Fi callback". The implementation in `ring_buffer.c` does **not** use a spinlock. It relies on:

* `__atomic_store_n(..., __ATOMIC_RELEASE)` on `tail` (line 38)
* `__atomic_load_n(..., __ATOMIC_ACQUIRE)` on `tail` in `rb_pop` (line 49)

This works for **single-producer / single-consumer**. The firmware has **two producers running on the Wi-Fi task**:

* `promisc_cb` (`wifi_sensor.c:63`) → `rb_push`
* `csi_cb` (`wifi_sensor.c:119`) → `rb_push`

If both callbacks fire from the same Wi-Fi task they will not race against each other (single thread), but on ESP32 (dual-core) the Wi-Fi/CSI stack can invoke RX callbacks from different internal task contexts in some configurations, and `xTaskCreatePinnedToCore(... 1)` for the consumer (`main.c:39-45`) leaves the writer free of contention from itself but **not** between the two producer callbacks.

Specific races:
* `rb->dropped++` (line 27): plain RMW from two producers — lost increments under contention.
* `rb->high_watermark` update (lines 40-41): read-compare-write — same problem.
* `next_tail = (rb->tail + 1) % RB_SLOT_COUNT` then `slot = &rb->slots[rb->tail]` then `__atomic_store_n(&rb->tail, next_tail, …)`: between the read of `tail` and the atomic store, a second producer reading the same `tail` will write into the same slot and both will publish the same `next_tail`, corrupting one record and leaving one slot uninitialized.

Either:
* take a `portMUX_TYPE` spinlock around the whole `rb_push` body (cheap, ISR-safe), **or**
* document hard that *all* `rb_push` callers run in one task only and verify that empirically.

The current code does neither.

### 3.3 🟠 512-byte stack buffer in CSI callback

`wifi_sensor.c:137` allocates `uint8_t slot[RB_SLOT_SIZE]` (= 512 B) on the
stack of the Wi-Fi CSI callback. Wi-Fi callbacks run in the Wi-Fi driver's
internal task, which by default has a stack on the order of 3072 B on
ESP32-S2 (configurable via `CONFIG_ESP32_WIFI_TASK_STACK_SIZE` / equivalent for
S2). 512 B is ~17% of the budget, plus the callback's own frame, plus the CRC
loop, plus driver call depth. It is not *guaranteed* to overflow, but it is the
kind of latent bug that surfaces under heavy CSI load or after a future ESP-IDF
update tightens stack defaults.

Two safer alternatives:
1. Reserve a static `uint8_t s_csi_scratch[RB_SLOT_SIZE]` and document
   "single-producer for CSI callback".
2. Add an `rb_reserve_slot(...)` API that hands back a pointer to the
   ring-buffer slot directly so the callback never needs a scratch buffer.

### 3.4 🟠 `rb_used()` is not a safe high-watermark source

`ring_buffer.c:40-41` calls `rb_used(rb)` from inside `rb_push` to compute the
high watermark. `rb_used` reads `tail` then `head` non-atomically and is itself
called in a producer that has *just* advanced `tail`. With two producers (see
§3.2) or with the consumer racing, the value returned can transiently report
either 0 or `RB_SLOT_COUNT-1` (wrap-around). The watermark will then either
under-report or jump to its maximum value spuriously and stick there.

### 3.5 🟡 STATUS record reports compile-time channel, not actual

`serial_writer.c:83` sets `.channel = SENSOR_WIFI_CHANNEL`. The task spec says
the status record should reflect the current channel; if a future change adds
channel hopping, this field will silently lie. Cheap fix: query
`esp_wifi_get_channel(&primary, &second)` once at init and cache the value, or
read it on each status emission.

### 3.6 🟡 Promiscuous filter accepts data frames even when `CAPTURE_DATA_FRAMES=0`

`wifi_sensor.c:179-183` always installs a filter mask of `MGMT | DATA`. If
`CAPTURE_DATA_FRAMES` is set to 0 at compile time, data frames are still
delivered to the callback and discarded there (`wifi_sensor.c:89-93`). This
wastes Wi-Fi RX buffers and callback CPU. The filter should be assembled from
the same macros.

### 3.7 🟡 `volatile` is used as a synchronization primitive

`g_rssi_records_sent`, `g_csi_records_sent`, `s_rssi_seq`, `s_csi_seq`,
`rb->dropped`, `rb->high_watermark` are all declared `volatile`. `volatile`
prevents the compiler from caching the value, but it does **not** provide
atomicity across cores. The sequence counters are only ever incremented from
the (effectively single) Wi-Fi callback thread today, so the bug is latent, but
the status-record reader on core 1 may observe torn 32-bit values on platforms
where unaligned 32-bit access is not atomic. ESP32 (Xtensa) 32-bit aligned
loads/stores are atomic, so this is mostly a documentation/style issue — but
using `_Atomic uint32_t` or `__atomic_load_n` would make the intent explicit.

### 3.8 🟡 CRC duplicated in two translation units

`crc16_ccitt` is defined identically in `wifi_sensor.c:25-35` and
`serial_writer.c:23-33`. Move to a shared `crc16.{c,h}` to eliminate drift
risk. Low priority but trivial to fix.

### 3.9 🟡 `serial_writer_send_raw` declared but never used

`serial_writer.h:21` exposes `serial_writer_send_raw` but no caller in the
project uses it; `send_status_record` calls `uart_write_bytes` directly
(`serial_writer.c:88`). Either use it from `send_status_record` for consistency
or remove from the header.

### 3.10 🟢 `collector.py` byte-by-byte resync is slow

`tools/collector.py:86-100` does a one-byte `port.read(1)` per iteration. At
921600 baud with a noisy initial buffer this is fine but inefficient. For a
future revision, read in chunks and scan for `0x5a 0xa5`.

### 3.11 🟢 `collector.py` status `channel` field decoded as signed (`b`)

`STATUS_FMT = "<HBBIIIIIbH"` uses `b` (signed int8) for `channel`. The C side
declares `uint8_t channel` (`record_protocol.h:68`). Use `B` for symmetry.
Practically harmless for channels 1–14.

### 3.12 🟢 README claims logs are redirected to UART1; code only suppresses

`README.md:48-50` and `serial_writer.h:11` say logs go to UART1. The code calls
`esp_log_set_vprintf(NULL)` (`serial_writer.c:60`), which *suppresses* all log
output. There is no UART1 setup anywhere. Either implement UART1 routing or
update the docs to say logs are silently dropped when `SENSOR_DEBUG_LOGS=0`.

### 3.13 🟢 `WRITER_UART_TX/RX = 1/3` hard-codes ESP32 pins

ESP32-S2's default UART0 pins are GPIO 43/44, not 1/3. Using
`UART_PIN_NO_CHANGE` for both would let the bootloader's wiring stand on both
chips. Today's code will likely still work because the call is followed by
`uart_param_config` and the driver re-checks, but the literal values mislead
readers.

### 3.14 🟢 `ESP_LOGI` in init still emits even when `SENSOR_DEBUG_LOGS=0`?

`main.c:35` and `wifi_sensor.c:203` print `ESP_LOGI` lines. By the time these
run, `esp_log_set_vprintf(NULL)` has already been called (`main.c:30`), so the
logs are silently dropped — good. But the `ESP_LOGI` in `serial_writer_init`
itself (`serial_writer.c:63`) is emitted *after* `esp_log_set_vprintf(NULL)` is
set within the same function, so it is also suppressed. Documenting this
ordering invariant in a comment would protect against future refactors.

---

## 4. Strengths worth keeping

* Clean module separation: protocol header, ring buffer, sensor, writer, main.
* Compile-time feature flags exactly as `task1.md` lists them (`sensor_config.h:9-51`).
* Init order matches the spec verbatim (`main.c` + `wifi_sensor_init`).
* Callbacks do no formatting and no UART I/O — only a memcpy + push.
* Status record carries every metric the task asks for: uptime, RSSI/CSI sent, dropped, queue HWM, channel.
* Python collector pretty-prints all three record types and writes CSV — minimum viable host-side tool.
* README structure follows the deliverable list in `task1.md`: boards, build, flash, monitor, channel, protocol, Python example, limitations.
* CRC-16/CCITT-FALSE chosen; both ends implement the same polynomial and seed.

---

## 5. Recommended fix order

1. **Fix the documented sizes everywhere** (§3.1). One PR: `record_protocol.h` comments + README tables (recompute every offset) + `tools/collector.py` asserts. Without this the collector cannot start.
2. **Make `rb_push` multi-producer safe** (§3.2). Take a `portMUX_TYPE` and wrap the whole body. Update the header comment to actually describe what is done.
3. **Replace the 512-byte stack buffer in `csi_cb`** with a static scratch buffer or a slot-reservation API (§3.3).
4. Tighten the promiscuous filter mask (§3.6) and report the actual channel in status (§3.5) — cheap.
5. Consolidate CRC (§3.8), tidy unused symbols (§3.9), and reconcile log routing docs (§3.12). Pure hygiene.

After (1)–(3), every acceptance criterion in `task1.md §Acceptance Criteria` should be reachable. The remaining items are quality-of-life and have no impact on the first-test scenario.

---

## 6. Files reviewed

```
CMakeLists.txt
sdkconfig.defaults
README.md
main/CMakeLists.txt
main/main.c
main/sensor_config.h
main/record_protocol.h
main/wifi_sensor.h
main/wifi_sensor.c
main/ring_buffer.h
main/ring_buffer.c
main/serial_writer.h
main/serial_writer.c
tools/collector.py
```

No build was attempted (review-only).
