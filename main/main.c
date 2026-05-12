#include "wifi_sensor.h"
#include "serial_writer.h"
#include "sensor_config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"

static const char *TAG = "main";

void app_main(void)
{
    /* 1. NVS flash (required by Wi-Fi driver). */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. Network interface and event loop (required before esp_wifi_init). */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    /* 3. Serial writer (configures UART0, optionally silences logs). */
    serial_writer_init();

    /* 4. Wi-Fi sensor (promiscuous + CSI, pushes into ring buffer). */
    wifi_sensor_init();

    ESP_LOGI(TAG, "sensor started: channel=%d baud=%d",
             SENSOR_WIFI_CHANNEL, SENSOR_BAUD_RATE);

    /* 5. Serial writer task – single consumer draining ring buffer. */
    xTaskCreatePinnedToCore(serial_writer_task,
                            "serial_writer",
                            4096,   /* stack bytes */
                            NULL,
                            5,      /* priority    */
                            NULL,
                            1);     /* pin to core 1, leaving core 0 for Wi-Fi */
}
