#include "serial_writer.h"
#include "ring_buffer.h"
#include "record_protocol.h"
#include "sensor_config.h"
#include "crc16.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "esp_log.h"

static const char *TAG = "serial_writer";

/* Shared ring buffer – declared extern; defined in wifi_sensor.c */
extern ring_buffer_t g_ring_buffer;

/* Stats counters – declared extern; defined in wifi_sensor.c */
extern volatile uint32_t g_rssi_records_sent;
extern volatile uint32_t g_csi_records_sent;

/* ── UART helpers ───────────────────────────────────────────────────────── */
#define WRITER_UART_PORT UART_NUM_0

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
    /*
     * Use UART_PIN_NO_CHANGE for all pins so the bootloader's pin assignment
     * is preserved on both ESP32 (GPIO1/3) and ESP32-S2 (GPIO43/44).
     */
    ESP_ERROR_CHECK(uart_set_pin(WRITER_UART_PORT,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    /* TX buffer large enough to absorb bursts; no RX buffer needed. */
    ESP_ERROR_CHECK(uart_driver_install(WRITER_UART_PORT,
                                        0, 4096, 0, NULL, 0));

#if !SENSOR_DEBUG_LOGS
    /*
     * Suppress all ESP-IDF log output so UART0 carries only binary records.
     * This must happen BEFORE any subsequent ESP_LOGx calls in this
     * translation unit; callers relying on logs must set SENSOR_DEBUG_LOGS=1.
     */
    esp_log_set_vprintf(NULL);  /* silently drops all log output */
#endif

    /* If SENSOR_DEBUG_LOGS=0 this line is already suppressed by the call
     * to esp_log_set_vprintf(NULL) above. */
    ESP_LOGI(TAG, "serial writer ready, baud=%d", SENSOR_BAUD_RATE);
}

void serial_writer_send_raw(const void *data, size_t len)
{
    uart_write_bytes(WRITER_UART_PORT, data, len);
}

/* ── Status record builder ──────────────────────────────────────────────── */
static void send_status_record(void)
{
    /* Query the actual current channel rather than the compile-time default. */
    uint8_t ch = SENSOR_WIFI_CHANNEL, sec = 0;
    esp_wifi_get_channel(&ch, &sec);

    status_record_t r = {
        .magic                = PROTO_MAGIC,
        .version              = PROTO_VERSION,
        .record_type          = RECORD_TYPE_STATUS,
        .uptime_ms            = (uint32_t)(esp_timer_get_time() / 1000),
        .rssi_records_sent    = g_rssi_records_sent,
        .csi_records_sent     = g_csi_records_sent,
        .records_dropped      = g_ring_buffer.dropped,
        .queue_high_watermark = g_ring_buffer.high_watermark,
        .channel              = ch,
        .crc16                = 0,
    };
    r.crc16 = crc16_ccitt((const uint8_t *)&r,
                           sizeof(r) - sizeof(r.crc16));
    serial_writer_send_raw(&r, sizeof(r));
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
