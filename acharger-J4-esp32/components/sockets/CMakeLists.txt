idf_component_register(SRCS "esp32_sockets.c"
                      INCLUDE_DIRS "."
                      REQUIRES esp_event esp_netif lwip nvs_flash)
