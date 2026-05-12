#pragma once
#include <stdint.h>

/*
 * Binary stream protocol for ESP32 Wi-Fi RSSI/CSI sensor.
 *
 * All multi-byte integers are little-endian.
 * Every record starts with magic 0xA55A and ends with crc16 (CRC-16/CCITT-FALSE).
 *
 * Record types:
 *   1 = RSSI_RECORD
 *   2 = CSI_RECORD   (header followed by csi_len raw bytes)
 *   3 = STATUS_RECORD
 *
 * MAC field in management frames: transmitter address (TA) / BSSID.
 *   Beacon          -> addr2 (BSSID / transmitter)
 *   Probe response  -> addr2 (BSSID / transmitter)
 *   Data frame      -> addr2 (transmitter / source address)
 */

#define PROTO_MAGIC   0xA55A
#define PROTO_VERSION 1

#define RECORD_TYPE_RSSI   1
#define RECORD_TYPE_CSI    2
#define RECORD_TYPE_STATUS 3

/* ── RSSI record (no trailing payload) ──────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint16_t magic;          /* 0xA55A                          */
    uint8_t  version;        /* PROTO_VERSION                   */
    uint8_t  record_type;    /* RECORD_TYPE_RSSI = 1            */
    uint32_t seq;            /* monotonically increasing        */
    uint32_t timestamp_us;   /* esp_timer_get_time() & 0xFFFFFFFF */
    uint8_t  channel;
    int8_t   rssi;
    uint8_t  frame_type;     /* wifi_promiscuous_pkt_type_t     */
    uint8_t  frame_subtype;  /* 802.11 subtype nibble           */
    uint8_t  mac[6];         /* see MAC Handling note above     */
    uint16_t payload_len;    /* 0 for RSSI-only records         */
    uint16_t crc16;
} rssi_record_t;             /* total: 26 bytes                 */

/* ── CSI record header (followed by csi_len raw bytes) ─────────────────── */
typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t  version;
    uint8_t  record_type;    /* RECORD_TYPE_CSI = 2             */
    uint32_t seq;
    uint32_t timestamp_us;
    uint8_t  channel;
    int8_t   rssi;
    uint8_t  mac[6];
    uint16_t csi_len;        /* number of raw CSI bytes that follow */
    uint16_t crc16;          /* covers header only              */
} csi_record_header_t;       /* total: 24 bytes                 */

/* ── Status record ──────────────────────────────────────────────────────── */
typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint8_t  version;
    uint8_t  record_type;    /* RECORD_TYPE_STATUS = 3          */
    uint32_t uptime_ms;
    uint32_t rssi_records_sent;
    uint32_t csi_records_sent;
    uint32_t records_dropped;
    uint32_t queue_high_watermark;
    uint8_t  channel;
    uint16_t crc16;
} status_record_t;           /* total: 27 bytes                 */
