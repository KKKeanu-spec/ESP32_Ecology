idf_component_register(SRCS "esp32_wifi.c" 
                       INCLUDE_DIRS "."
                       REQUIRES nvs_flash lwip uart esp_event esp_netif)
