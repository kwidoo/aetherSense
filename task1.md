# Copilot Task: ESP32 RSSI/CSI Wi-Fi Sensing Firmware

## Goal

Create an ESP-IDF firmware for an ESP32/ESP32-S2-based board that passively collects Wi-Fi RSSI and CSI data from 2.4 GHz packets and streams the data to a host computer over USB serial.

Primary target board:

- Flipper Zero Wi-Fi Devboard or compatible ESP32-S2 board
- USB serial output to Mac/Linux/PC
- 2.4 GHz only
- Fixed Wi-Fi channel for the first implementation

The firmware must be minimal, stable, and focused on data capture. Heavy processing, storage, visualization, and statistical analysis must happen on the host computer, not on the ESP32.

---

## Context

The experiment setup has:

- One ESP32/ESP32-S2 sensor placed near the center of the room.
- Three controlled OpenWrt APs configured on the same 2.4 GHz channel.
- Optional neighboring APs visible on the same channel.
- The ESP32 should listen passively and collect RSSI/CSI from received packets.
- The first goal is to build a stable dataset for later baseline/delta/statistical analysis.

---

## Hard Requirements

### 1. Use ESP-IDF

Use ESP-IDF, not Arduino, unless the existing project is already Arduino-based.

The firmware must compile with a standard ESP-IDF toolchain.

---

### 2. Fixed Channel Passive Sniffer

Implement passive Wi-Fi sniffing on a fixed 2.4 GHz channel.

Configuration must allow setting:

```c
#define SENSOR_WIFI_CHANNEL 6
````

or equivalent via `menuconfig`.

The firmware must not perform channel hopping in the first version.

---

### 3. Promiscuous Mode

Enable Wi-Fi promiscuous mode.

The firmware must capture at least:

* Beacon frames
* Probe responses if available
* Data frames if they are useful for CSI
* Source/BSSID MAC
* RSSI
* Channel
* Timestamp
* Frame type/subtype

The firmware should preferably allow filtering frame types via compile-time flags.

Example desired flags:

```c
#define CAPTURE_BEACONS 1
#define CAPTURE_PROBE_RESPONSES 1
#define CAPTURE_DATA_FRAMES 1
```

---

### 4. RSSI Record Output

For every accepted packet, output a compact RSSI record over USB serial.

Do not use JSON for high-frequency output.

Use a binary protocol.

Required fields:

```c
struct rssi_record {
    uint16_t magic;        // e.g. 0xA55A
    uint8_t  version;      // protocol version
    uint8_t  record_type;  // RSSI = 1
    uint32_t seq;
    uint32_t timestamp_us;
    uint8_t  channel;
    int8_t   rssi;
    uint8_t  frame_type;
    uint8_t  frame_subtype;
    uint8_t  mac[6];       // transmitter/source/BSSID, documented clearly
    uint16_t payload_len;  // 0 for RSSI-only records
    uint16_t crc16;        // optional but preferred
};
```

If exact struct packing differs, document it clearly.

---

### 5. CSI Capture

Enable CSI capture using ESP-IDF CSI APIs.

Implement CSI callback and output CSI records over USB serial.

Important:

* Do not do heavy work inside the CSI callback.
* Copy data quickly into a queue/ring buffer.
* Serialize and send from a separate FreeRTOS task.

Required CSI record fields:

```c
struct csi_record_header {
    uint16_t magic;        // e.g. 0xA55A
    uint8_t  version;
    uint8_t  record_type;  // CSI = 2
    uint32_t seq;
    uint32_t timestamp_us;
    uint8_t  channel;
    int8_t   rssi;
    uint8_t  mac[6];
    uint16_t csi_len;
    uint16_t crc16;        // optional but preferred
    // followed by raw CSI bytes
};
```

The raw CSI payload should be transmitted as received from ESP-IDF unless there is a strong reason to transform it.

---

### 6. Queue / Ring Buffer

Implement a bounded queue or ring buffer between Wi-Fi callbacks and serial output.

The firmware must track dropped records.

Expose periodic status records, for example once per second:

```c
struct status_record {
    uint16_t magic;
    uint8_t  version;
    uint8_t  record_type; // STATUS = 3
    uint32_t uptime_ms;
    uint32_t rssi_records_sent;
    uint32_t csi_records_sent;
    uint32_t records_dropped;
    uint32_t queue_high_watermark;
    uint8_t  channel;
};
```

---

### 7. Serial Output

Send all records over USB serial/UART.

Required:

* Configurable baud rate.
* Default: `921600`.
* Output must be binary-safe.
* Do not print logs into the same binary stream unless debug mode is enabled.

Provide a way to disable ESP-IDF logs or redirect them away from the binary stream.

---

### 8. Minimal CLI / Configuration

On boot, print a short human-readable startup line only if debug mode is enabled.

Useful configuration options:

```c
SENSOR_WIFI_CHANNEL=6
SENSOR_BAUD_RATE=921600
ENABLE_RSSI=1
ENABLE_CSI=1
ENABLE_STATUS_RECORDS=1
STATUS_INTERVAL_MS=1000
CAPTURE_BEACONS=1
CAPTURE_PROBE_RESPONSES=1
CAPTURE_DATA_FRAMES=1
```

Prefer `menuconfig`, but compile-time constants are acceptable for the first version.

---

## Implementation Notes

### Wi-Fi Init

The firmware should:

1. Initialize NVS.
2. Initialize network interface if required by ESP-IDF.
3. Initialize Wi-Fi in station mode.
4. Set storage to RAM.
5. Set fixed channel.
6. Enable promiscuous mode.
7. Register promiscuous RX callback.
8. Configure CSI.
9. Register CSI callback.
10. Enable CSI.

---

### Callback Safety

Do not:

* Allocate large memory repeatedly inside callbacks.
* Format JSON inside callbacks.
* Write directly to UART from CSI callback.
* Block inside callbacks.
* Use slow logging inside callbacks.

Do:

* Copy minimal metadata.
* Copy CSI payload into preallocated/bounded buffer.
* Increment drop counters if queue is full.
* Return quickly.

---

### MAC Handling

For management frames, extract BSSID/source MAC consistently.

Document which MAC is stored:

* Beacon: BSSID or transmitter address
* Probe response: BSSID or transmitter address
* Data frame: transmitter/source address

The collector can later group by this MAC.

---

### CSI Payload

Keep raw CSI payload intact.

Do not attempt to interpret CSI on the ESP32 in this task.

Host-side analysis will later handle:

* amplitude
* phase/real-imag conversion
* baseline
* delta
* variance
* z-score
* event detection

---

## Deliverables

### Firmware

Create or update the ESP-IDF project with:

```text
main/
  main.c
  wifi_sensor.c
  wifi_sensor.h
  record_protocol.h
  serial_writer.c
  serial_writer.h
  ring_buffer.c or queue wrapper
  ring_buffer.h
CMakeLists.txt
sdkconfig.defaults
README.md
```

Exact file structure may differ, but responsibilities must be separated clearly.

---

### README

Add a README with:

1. Supported boards.
2. Build instructions.
3. Flash instructions.
4. Monitor instructions.
5. How to set Wi-Fi channel.
6. Binary protocol description.
7. Example Python command to read serial.
8. Known limitations.

Example commands:

```bash
idf.py set-target esp32s2
idf.py build
idf.py -p /dev/tty.usbmodemXXXX flash
```

or for ESP32:

```bash
idf.py set-target esp32
idf.py build
idf.py -p /dev/ttyUSB0 flash
```

---

### Python Collector Stub

Add a minimal Python script:

```text
tools/collector.py
```

It should:

* Open serial port.
* Read binary records.
* Decode RSSI records.
* Decode CSI headers and skip or store raw CSI payload.
* Print status summary.
* Optionally write CSV for RSSI records.

Example usage:

```bash
python tools/collector.py --port /dev/tty.usbmodemXXXX --baud 921600 --out rssi.csv
```

The collector does not need full CSI analysis yet.

---

## Acceptance Criteria

The task is complete when:

1. Firmware builds successfully for ESP32-S2.
2. Firmware can be flashed to the Flipper Wi-Fi Devboard or compatible ESP32-S2 board.
3. Device boots and sets a fixed 2.4 GHz channel.
4. Device captures beacon RSSI records from visible APs.
5. Device streams binary RSSI records over USB serial.
6. CSI callback is implemented and can emit CSI records when CSI data is available.
7. No heavy processing is done inside Wi-Fi callbacks.
8. Dropped records are counted.
9. Status records are emitted periodically.
10. Python collector can read and decode at least RSSI and STATUS records.
11. README documents protocol and usage.

---

## Out of Scope

Do not implement yet:

* Channel hopping.
* Web UI.
* MQTT.
* HTTP server.
* Local database on ESP32.
* Machine learning.
* Baseline/statistical analysis on ESP32.
* Multi-sensor synchronization.
* Directional antenna/motor support.
* OpenWrt router integration.
* Visualization dashboard.

---

## Preferred First Test Scenario

Use this setup:

```text
AP1: 2.4 GHz channel 6, fixed TX power
AP2: 2.4 GHz channel 6, fixed TX power
AP3: 2.4 GHz channel 6, fixed TX power
ESP32 sensor: fixed channel 6
```

Run:

```bash
python tools/collector.py --port /dev/tty.usbmodemXXXX --baud 921600 --out rssi.csv
```

Expected result:

* RSSI records from all three AP BSSIDs.
* Stable sequence numbers.
* Periodic status records.
* Low or zero dropped records in RSSI-only mode.

