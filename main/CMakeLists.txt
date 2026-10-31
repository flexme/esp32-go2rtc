idf_component_register(SRCS "app.c" "config_store.c" "cloud.c" "miss.c" "rtc.c" "web.c" "hevc.c" "crypto_utils.c" "metrics.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif esp_http_server esp_http_client esp-tls nvs_flash esp_timer cjson mbedtls libsodium peer lwip esp_driver_uart
    EMBED_TXTFILES "www/index.html" "www/diagnostics.js" "$ENV{IDF_PATH}/components/mbedtls/esp_crt_bundle/cacrt_all.pem")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-unused-parameter)
