idf_component_register(
    SRCS
        "main.c"
        "wifi_sensor.c"
        "serial_writer.c"
        "ring_buffer.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        driver
        esp_timer
)
