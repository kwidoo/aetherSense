#pragma once

/*
 * Compile-time sensor configuration.
 * Override any value here or via CMake -D flags.
 * These values can also be set through Kconfig/menuconfig if desired.
 */

#ifndef SENSOR_WIFI_CHANNEL
#define SENSOR_WIFI_CHANNEL       6
#endif

#ifndef SENSOR_BAUD_RATE
#define SENSOR_BAUD_RATE          921600
#endif

#ifndef ENABLE_RSSI
#define ENABLE_RSSI               1
#endif

#ifndef ENABLE_CSI
#define ENABLE_CSI                1
#endif

#ifndef ENABLE_STATUS_RECORDS
#define ENABLE_STATUS_RECORDS     1
#endif

#ifndef STATUS_INTERVAL_MS
#define STATUS_INTERVAL_MS        1000
#endif

#ifndef CAPTURE_BEACONS
#define CAPTURE_BEACONS           1
#endif

#ifndef CAPTURE_PROBE_RESPONSES
#define CAPTURE_PROBE_RESPONSES   1
#endif

#ifndef CAPTURE_DATA_FRAMES
#define CAPTURE_DATA_FRAMES       1
#endif

/*
 * Set to 1 to enable ESP-IDF log output.
 * Set to 0 to suppress all logs via esp_log_set_vprintf(NULL) and keep UART0 binary-clean.
 */
#ifndef SENSOR_DEBUG_LOGS
#define SENSOR_DEBUG_LOGS         0
#endif
