idf_component_register(
    SRCS "wifi_mgr.cpp" "ota.cpp" "watch_http.cpp" "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_http_client
        esp_http_server
        esp-tls
        mbedtls
        app_update
        esp_app_format
        driver
        esp_timer
        esp_adc
        json
)
