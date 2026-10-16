idf_component_register(SRCS "esp32_tls.c"
                      INCLUDE_DIRS "."
                      REQUIRES esp_event esp_netif esp-tls mbedtls nvs_flash)
