idf_component_register(
    SRCS "net_client.c" "net_report.c"
    INCLUDE_DIRS "."
    REQUIRES tinyml nvs_flash lwip esp_timer esp_wifi
)
