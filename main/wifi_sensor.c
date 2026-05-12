#include "wifi_sensor.h"
#include "ring_buffer.h"
#include "record_protocol.h"
#include "sensor_config.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_wifi.h"
#include "esp_wifi_types.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "wifi_sensor";

/* ── Shared state (accessed by serial_writer.c) ─────────────────────────── */
ring_buffer_t        g_ring_buffer;
volatile uint32_t    g_rssi_records_sent = 0;
volatile uint32_t    g_csi_records_sent  = 0;

static volatile uint32_t s_rssi_seq = 0;
static volatile uint32_t s_csi_seq  = 0;

/* ── CRC-16/CCITT-FALSE (duplicated to keep modules self-contained) ──────── */
static uint16_t crc16_ccitt(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
        }
    }
    return crc;
}

/* ── 802.11 frame header helpers ────────────────────────────────────────── */
/*
 * Minimal 802.11 MAC header layout (fixed portion):
 *   [0]  frame_ctrl byte 0   (type/subtype)
 *   [1]  frame_ctrl byte 1
 *   [2-3] duration
 *   [4-9] addr1 (DA / RA)
 *   [10-15] addr2 (SA / TA / BSSID)  <- we capture this
 *   [16-21] addr3
 */
#define DOT11_HDR_ADDR2_OFFSET  10

static void extract_mac(const uint8_t *payload, uint16_t payload_len, uint8_t mac[6])
{
    if (payload_len >= DOT11_HDR_ADDR2_OFFSET + 6) {
        memcpy(mac, payload + DOT11_HDR_ADDR2_OFFSET, 6);
    } else {
        memset(mac, 0, 6);
    }
}

/* ── Promiscuous callback ────────────────────────────────────────────────── */
/*
 * Called by the Wi-Fi driver in its internal task context.
 * Must return quickly – no heap alloc, no UART writes, no logging.
 */
static void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
#if !ENABLE_RSSI
    return;
#endif

    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const wifi_pkt_rx_ctrl_t     *rx  = &pkt->rx_ctrl;

    /* Frame type/subtype from 802.11 FC byte 0: bits[7:4]=subtype, bits[3:2]=type */
    uint8_t fc0      = (pkt->payload_len > 0) ? pkt->payload[0] : 0;
    uint8_t ftype    = (fc0 >> 2) & 0x03;
    uint8_t fsubtype = (fc0 >> 4) & 0x0F;

    /* Apply compile-time capture filters. */
    if (type == WIFI_PKT_MGMT) {
        /* subtype 8 = beacon, subtype 5 = probe response */
        bool is_beacon        = (fsubtype == 8);
        bool is_probe_resp    = (fsubtype == 5);
#if !CAPTURE_BEACONS
        if (is_beacon) return;
#endif
#if !CAPTURE_PROBE_RESPONSES
        if (is_probe_resp) return;
#endif
        (void)is_beacon; (void)is_probe_resp;
    } else if (type == WIFI_PKT_DATA) {
#if !CAPTURE_DATA_FRAMES
        return;
#endif
    }

    rssi_record_t r = {
        .magic        = PROTO_MAGIC,
        .version      = PROTO_VERSION,
        .record_type  = RECORD_TYPE_RSSI,
        .seq          = s_rssi_seq++,
        .timestamp_us = (uint32_t)(esp_timer_get_time() & 0xFFFFFFFF),
        .channel      = (uint8_t)rx->channel,
        .rssi         = (int8_t)rx->rssi,
        .frame_type   = ftype,
        .frame_subtype= fsubtype,
        .payload_len  = 0,
        .crc16        = 0,
    };
    extract_mac(pkt->payload, pkt->payload_len, r.mac);
    r.crc16 = crc16_ccitt((const uint8_t *)&r, sizeof(r) - sizeof(r.crc16));

    rb_push(&g_ring_buffer, &r, sizeof(r));
}

/* ── CSI callback ────────────────────────────────────────────────────────── */
/*
 * Called by the Wi-Fi driver in its internal task context.
 * We copy header + raw CSI bytes into one ring-buffer slot and return.
 */
static void csi_cb(void *ctx, wifi_csi_info_t *data)
{
#if !ENABLE_CSI
    return;
#endif
    if (!data || !data->buf) return;

    uint16_t csi_len = (uint16_t)data->len;
    uint16_t total   = (uint16_t)(sizeof(csi_record_header_t) + csi_len);

    if (total > RB_SLOT_SIZE) {
        /* Payload too large for one slot – drop and count. */
        g_ring_buffer.dropped++;
        return;
    }

    /* Build header + payload into a stack buffer.
     * Stack frame is safe here: csi_len <= RB_SLOT_SIZE - header. */
    uint8_t slot[RB_SLOT_SIZE];

    csi_record_header_t *hdr = (csi_record_header_t *)slot;
    hdr->magic        = PROTO_MAGIC;
    hdr->version      = PROTO_VERSION;
    hdr->record_type  = RECORD_TYPE_CSI;
    hdr->seq          = s_csi_seq++;
    hdr->timestamp_us = (uint32_t)(esp_timer_get_time() & 0xFFFFFFFF);
    hdr->channel      = (uint8_t)data->rx_ctrl.channel;
    hdr->rssi         = (int8_t)data->rx_ctrl.rssi;
    hdr->csi_len      = csi_len;
    hdr->crc16        = 0;
    memcpy(hdr->mac, data->mac, 6);

    /* Copy raw CSI payload immediately after the header. */
    memcpy(slot + sizeof(csi_record_header_t), data->buf, csi_len);

    /* CRC covers header only. */
    hdr->crc16 = crc16_ccitt((const uint8_t *)hdr,
                              sizeof(csi_record_header_t) - sizeof(hdr->crc16));

    rb_push(&g_ring_buffer, slot, total);
}

/* ── Public init ─────────────────────────────────────────────────────────── */
void wifi_sensor_init(void)
{
    rb_init(&g_ring_buffer);

    /* 1. Wi-Fi driver init (netif already initialised in main). */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* 2. Station mode, RAM storage only. */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* 3. Fix to the configured channel (primary, no HT40 secondary). */
    ESP_ERROR_CHECK(esp_wifi_set_channel(SENSOR_WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE));

    /* 4. Promiscuous mode. */
    wifi_promiscuous_filter_t filter = {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT |
                       WIFI_PROMIS_FILTER_MASK_DATA,
    };
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_filter(&filter));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous_rx_cb(promisc_cb));
    ESP_ERROR_CHECK(esp_wifi_set_promiscuous(true));

#if ENABLE_CSI
    /* 5. CSI configuration and callback. */
    wifi_csi_config_t csi_cfg = {
        .lltf_en           = true,
        .htltf_en          = true,
        .stbc_htltf2_en    = true,
        .ltf_merge_en      = true,
        .channel_filter_en = false,
        .manu_scale        = false,
        .shift              = 0,
    };
    ESP_ERROR_CHECK(esp_wifi_set_csi_config(&csi_cfg));
    ESP_ERROR_CHECK(esp_wifi_set_csi_rx_cb(csi_cb, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_csi(true));
#endif

    ESP_LOGI(TAG, "Wi-Fi sensor ready on channel %d", SENSOR_WIFI_CHANNEL);
}
