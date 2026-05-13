# ESP32 Wi-Fi RSSI/CSI Sensor

Minimal ESP-IDF firmware that passively sniffs 2.4 GHz 802.11 frames and
streams compact binary RSSI, CSI, and STATUS records over USB serial.

---

## Supported Boards

| Board | Target |
|---|---|
| Flipper Zero Wi-Fi Devboard (ESP32-S2) | `esp32s2` |
| Generic ESP32 devkit | `esp32` |
| Generic ESP32-S2 devkit | `esp32s2` |

---

## Prerequisites

- ESP-IDF v5.x installed and `idf.py` on your `PATH`.
- Python 3.8+ with `pyserial` for the host collector.

---

## Build & Flash

### ESP32-S2 (Flipper Wi-Fi Devboard)

```bash
cd /path/to/this/project
idf.py set-target esp32s2
idf.py build
idf.py -p /dev/tty.usbmodemXXXX flash
```

### ESP32

```bash
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash
```

---

## Monitor (debug mode)

If `SENSOR_DEBUG_LOGS=1` is set, human-readable startup lines are printed on
boot.  Use a separate terminal and redirect to UART1 so the binary stream on
UART0 stays clean.

```bash
idf.py -p /dev/ttyUSB0 monitor
```

In production (`SENSOR_DEBUG_LOGS=0`, the default), ESP-IDF logs are suppressed
and UART0 carries only binary records.

---

## Set Wi-Fi Channel

Edit `main/sensor_config.h`:

```c
#define SENSOR_WIFI_CHANNEL 6
```

Or pass a CMake variable at build time (supported via `main/CMakeLists.txt`):

```bash
idf.py build -DSENSOR_WIFI_CHANNEL=11
```

---

## Binary Protocol

All records are **little-endian** and **packed** (no padding).

Every record begins with:

| Offset | Field | Type | Value |
|---|---|---|---|
| 0 | magic | uint16 | `0xA55A` |
| 2 | version | uint8 | `1` |
| 3 | record_type | uint8 | `1`=RSSI `2`=CSI `3`=STATUS |

### RSSI Record (26 bytes, record_type=1)

| Offset | Field | Type | Notes |
|---|---|---|---|
| 0 | magic | uint16 | 0xA55A |
| 2 | version | uint8 | |
| 3 | record_type | uint8 | 1 |
| 4 | seq | uint32 | monotonic |
| 8 | timestamp_us | uint32 | lower 32 bits of esp_timer_get_time() |
| 12 | channel | uint8 | Wi-Fi channel |
| 13 | rssi | int8 | dBm |
| 14 | frame_type | uint8 | 802.11 type nibble |
| 15 | frame_subtype | uint8 | 802.11 subtype nibble |
| 16 | mac | 6 bytes | addr2 (BSSID / transmitter) |
| 22 | payload_len | uint16 | 0 for RSSI records |
| 24 | crc16 | uint16 | CRC-16/CCITT-FALSE over bytes 0–23 |

**MAC field**: addr2 from 802.11 MAC header (bytes 10–15 of the MPDU).
- Beacon / probe response → BSSID (transmitter address)
- Data frame → transmitter / source address

### CSI Record (24-byte header + N raw bytes, record_type=2)

| Offset | Field | Type | Notes |
|---|---|---|---|
| 0 | magic | uint16 | |
| 2 | version | uint8 | |
| 3 | record_type | uint8 | 2 |
| 4 | seq | uint32 | |
| 8 | timestamp_us | uint32 | |
| 12 | channel | uint8 | |
| 13 | rssi | int8 | |
| 14 | mac | 6 bytes | |
| 20 | csi_len | uint16 | bytes of raw CSI payload that follow |
| 22 | crc16 | uint16 | covers header only (bytes 0–21) |
| 24 | *raw CSI* | csi_len bytes | as returned by ESP-IDF |

### STATUS Record (27 bytes, record_type=3)

| Offset | Field | Type | Notes |
|---|---|---|---|
| 0 | magic | uint16 | |
| 2 | version | uint8 | |
| 3 | record_type | uint8 | 3 |
| 4 | uptime_ms | uint32 | |
| 8 | rssi_records_sent | uint32 | |
| 12 | csi_records_sent | uint32 | |
| 16 | records_dropped | uint32 | ring-buffer overrun count |
| 20 | queue_high_watermark | uint32 | max simultaneous slots used |
| 24 | channel | uint8 | |
| 25 | crc16 | uint16 | covers bytes 0–24 |

---

## Python Collector

Install dependency:

```bash
pip install pyserial
```

Run:

```bash
python tools/collector.py --port /dev/tty.usbmodemXXXX --baud 921600 --out rssi.csv
```

Options:

| Flag | Default | Description |
|---|---|---|
| `--port` | (required) | Serial device path |
| `--baud` | `921600` | Baud rate |
| `--out` | none | CSV file for RSSI records |

The collector prints every decoded record to stdout and writes a status summary
once per STATUS record.  CSI records are decoded (header printed) but the raw
payload is not further processed in this version.

---

## Configuration Reference

All options are in `main/sensor_config.h`:

| Macro | Default | Description |
|---|---|---|
| `SENSOR_WIFI_CHANNEL` | `6` | Fixed 2.4 GHz channel |
| `SENSOR_BAUD_RATE` | `921600` | UART0 baud rate |
| `ENABLE_RSSI` | `1` | Enable RSSI records |
| `ENABLE_CSI` | `1` | Enable CSI records |
| `ENABLE_STATUS_RECORDS` | `1` | Enable periodic STATUS records |
| `STATUS_INTERVAL_MS` | `1000` | STATUS record interval |
| `CAPTURE_BEACONS` | `1` | Capture management beacon frames |
| `CAPTURE_PROBE_RESPONSES` | `1` | Capture probe-response frames |
| `CAPTURE_DATA_FRAMES` | `1` | Capture data frames |
| `SENSOR_DEBUG_LOGS` | `0` | Enable ESP-IDF log output |

---

## Known Limitations

- **Fixed channel only** – no channel hopping in this version.
- **ESP32-S2 CSI availability** – CSI from management frames is generally
  available; CSI from data frames requires associated stations.
- **timestamp_us rollover** – the timestamp field is the lower 32 bits of the
  64-bit ESP timer (rolls over every ~71 minutes).  The collector does not yet
  reconstruct the full 64-bit value.
- **Ring buffer size** – 128 × 512-byte slots (~64 KB).  Adjust `RB_SLOT_COUNT`
  and `RB_SLOT_SIZE` in `ring_buffer.h` for your heap budget.
- **Single UART output** – only UART0 is used.  A USB CDC ACM driver would
  provide true USB serial on ESP32-S2 but is not implemented here.
- **No channel on CSI record** – the channel field comes from `rx_ctrl.channel`;
  accuracy depends on the Wi-Fi driver state.

---

## Out of Scope (not implemented)

Channel hopping, Web UI, MQTT, HTTP server, local database, machine learning,
baseline/statistical analysis, multi-sensor sync, visualization dashboard.
