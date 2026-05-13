idf_component_register(
    SRCS
        "main.c"
        "wifi_sensor.c"
        "serial_writer.c"
        "ring_buffer.c"
        "crc16.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        driver
        esp_timer
)

# Allow build-time channel override: idf.py build -DSENSOR_WIFI_CHANNEL=11
if(DEFINED SENSOR_WIFI_CHANNEL)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE
        SENSOR_WIFI_CHANNEL=${SENSOR_WIFI_CHANNEL})
endif()
