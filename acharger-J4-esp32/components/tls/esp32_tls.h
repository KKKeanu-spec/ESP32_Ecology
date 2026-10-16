#pragma once

#include "esp_log.h"

/// @brief TLS初始化
/// @return 成功返回0，失败返回-1
int esp32_tls_init();

/// @brief TLS连接
/// @param server 服务器地址，IP或者域名，字符串格式
/// @param port 服务器端口号
/// @return 成功返回0，失败返回-1
int esp32_tls_connect(char *server, uint16_t port);

/// @brief 通过TLS发送数据
/// @param buf_send 数据指针
/// @param len 数据长度
/// @return 成功返回发送数据长度，失败返回-1
int esp32_tls_send_data(char *buf_send, uint16_t len);

/// @brief TLS接收数据（阻塞）
/// @param buf_read 缓存指针
/// @param len 最大缓存长度
/// @return 成功返回接收数据长度，失败返回-1
int esp32_tls_recv_data(char *buf_read, uint16_t len);

/// @brief 关闭TLS连接
/// @return 成功返回0，失败返回-1
int esp32_tls_exit();