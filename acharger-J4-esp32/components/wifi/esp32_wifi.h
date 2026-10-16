#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_log.h"

typedef struct __attribute__((__packed__)) static_ip_set
{
    uint8_t ip[4];
    uint8_t gw[4];
    uint8_t netmask[4];
    uint8_t dns[4];
} static_ip_set;

uint8_t get_wifi_state();

/// @brief WIFI初始化
/// @param  void
/// @return 成功返回0，失败返回-1
int esp32_wifi_init(void);

/// @brief 设置SSID
/// @param ssid SSID指针
/// @param len_ssid SSID长度
/// @return 成功返回0，失败返回-1
int esp32_wifi_set_ssid(char *ssid, uint8_t len_ssid);

/// @brief 设置密码
/// @param passwd 密码指针
/// @param len_passwd 密码长度
/// @return 成功返回0，失败返回-1
int esp32_wifi_set_passwd(char *passwd, uint8_t len_passwd);

/// @brief WIFI连接
/// @param  void
/// @return 成功返回0，失败返回-1
int esp32_wifi_connect(void);

/// @brief WIFI断开
/// @param  void
/// @return 成功返回0，失败返回-1
int esp32_wifi_disconnect(void);

/// @brief 设置WIFI静态IP
/// @param ip_set 静态IP参数指针
/// @return 成功返回0，失败返回-1
int esp32_wifi_static_ip(static_ip_set *ip_set);

/// @brief 开启DHCP功能
/// @return 成功返回0，失败返回-1
int esp32_wifi_dhcp_start();

/// @brief 设置是否在 WiFi 启动后立即自动连接
///        主机模式(等待 BLE 下发配置)应设为 false
/// @param enable true=自动连接, false=等待外部触发 esp32_wifi_connect()
void esp32_wifi_set_auto_connect(bool enable);
