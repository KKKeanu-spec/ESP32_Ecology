idf_component_register(SRCS "esp32_uart.c"
                       INCLUDE_DIRS "."
                       REQUIRES driver lwip ble wifi sockets tls)
