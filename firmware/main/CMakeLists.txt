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

# Build de TESTE de rollback: reprova o autoteste de propósito.
#   idf.py -B build-rollback-test -DROLLBACK_TEST_BUILD=1 build
# Use um diretório de build separado (a opção fica no cache do CMake).
# Nunca em produção: flash_device.py recusa e publish_firmware.py exige
# --allow-rollback-test para esse binário.
if(ROLLBACK_TEST_BUILD)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ROLLBACK_TEST_BUILD=1)
    message(WARNING "ROLLBACK_TEST_BUILD ativo: o firmware reprova o autoteste de propósito")
endif()
