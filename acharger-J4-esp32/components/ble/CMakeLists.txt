idf_component_register(SRCS "ble_app.c" "esp32_ble_server.c"
                       INCLUDE_DIRS "."
                       REQUIRES uart bt tinyml wifi net)
