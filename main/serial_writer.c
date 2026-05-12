#include "serial_writer.h"
#include "ring_buffer.h"
#include "record_protocol.h"
#include "sensor_config.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "serial_writer";

/* Shared ring buffer – declared extern; defined in wifi_sensor.c */
extern ring_buffer_t g_ring_buffer;

/* Stats counters – declared extern; defined in wifi_sensor.c */
extern volatile uint32_t g_rssi_records_sent;
extern volatile uint32_t g_csi_records_sent;

/* ── CRC-16/CCITT-FALSE ─────────────────────────────────────────────────── */
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

/* ── UART helpers ───────────────────────────────────────────────────────── */
#define WRITER_UART_PORT UART_NUM_0
#define WRITER_UART_TX   1  /* default TX pin */
#define WRITER_UART_RX   3  /* default RX pin */

void serial_writer_init(void)
{
    uart_config_t cfg = {
        .baud_rate  = SENSOR_BAUD_RATE,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(WRITER_UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(WRITER_UART_PORT,
                                 WRITER_UART_TX, WRITER_UART_RX,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    /* TX buffer large enough to absorb bursts; no RX buffer needed. */
    ESP_ERROR_CHECK(uart_driver_install(WRITER_UART_PORT,
                                        0, 4096, 0, NULL, 0));

#if !SENSOR_DEBUG_LOGS
    /* Redirect ESP-IDF log output away from UART0. */
    esp_log_set_vprintf(NULL);  /* suppress all logs */
#endif

    ESP_LOGI(TAG, "serial writer ready, baud=%d", SENSOR_BAUD_RATE);
}

void serial_writer_send_raw(const void *data, size_t len)
{
    uart_write_bytes(WRITER_UART_PORT, data, len);
}

/* ── Status record builder ──────────────────────────────────────────────── */
static void send_status_record(void)
{
    status_record_t r = {
        .magic                = PROTO_MAGIC,
        .version              = PROTO_VERSION,
        .record_type          = RECORD_TYPE_STATUS,
        .uptime_ms            = (uint32_t)(esp_timer_get_time() / 1000),
        .rssi_records_sent    = g_rssi_records_sent,
        .csi_records_sent     = g_csi_records_sent,
        .records_dropped      = g_ring_buffer.dropped,
        .queue_high_watermark = g_ring_buffer.high_watermark,
        .channel              = SENSOR_WIFI_CHANNEL,
        .crc16                = 0,
    };
    r.crc16 = crc16_ccitt((const uint8_t *)&r,
                           sizeof(r) - sizeof(r.crc16));
    uart_write_bytes(WRITER_UART_PORT, &r, sizeof(r));
}

/* ── Writer task ────────────────────────────────────────────────────────── */
void serial_writer_task(void *pvParameters)
{
    static uint8_t slot_buf[RB_SLOT_SIZE];
    uint16_t       slot_len  = 0;
    TickType_t     last_status = xTaskGetTickCount();

    for (;;) {
        bool got = rb_pop(&g_ring_buffer, slot_buf, &slot_len);
        if (got) {
            uart_write_bytes(WRITER_UART_PORT, slot_buf, slot_len);

            /* Classify record type to update counters. */
            if (slot_len >= 4) {
                uint8_t rtype = slot_buf[3];
                if (rtype == RECORD_TYPE_RSSI)  g_rssi_records_sent++;
                if (rtype == RECORD_TYPE_CSI)   g_csi_records_sent++;
            }
        } else {
            /* Nothing to drain – yield briefly. */
            vTaskDelay(pdMS_TO_TICKS(1));
        }

#if ENABLE_STATUS_RECORDS
        TickType_t now = xTaskGetTickCount();
        if ((now - last_status) >= pdMS_TO_TICKS(STATUS_INTERVAL_MS)) {
            send_status_record();
            last_status = now;
        }
#endif
    }
}
