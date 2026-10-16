#pragma once

#include "esp_log.h"

// 日志开关总览（置 0 关闭，置 1 打开）：
//   INF_LOG_ENABLE    : 常规流程日志（I 级，连接/任务创建/状态变化等，非每帧）
//   DATA_LOG_ENABLE   : 数据收发日志（I 级，每帧一条）。OTA 下载时帧数极多（269 包 × 多帧），
//                       默认关闭，避免刷屏淹没关键日志；需要逐帧排查时置 1。
//   HEX_LOG_ENABLE    : 数据十六进制 dump（W 级）。OTA 下载时每帧上 KB，默认关闭。
//   RECV_DEBUG_ENABLE : 串口收帧调试日志（W 级，RECV-DEBUG 前缀），默认关闭。
#define INF_LOG_ENABLE 1
#define INF_LOG_SIMPLE 0
#define DATA_LOG_ENABLE 1
#define HEX_LOG_ENABLE 0
#define RECV_DEBUG_ENABLE 0

enum
{
    STATE_OK, // 状态正常
    STATE_NO, // 状态异常
};

enum
{
    BLE_DATA, // 蓝牙数据
    NET_DATA, // wifi数据
    BLE_TEST, // 蓝牙测试专用
};

enum
{
    NET_SOCKET, // SOCKET传输
    NET_TLS     // TLS传输
};

enum
{
    /*
    命名规则:
    端口_方向_对象_动作_其他
    方向:
    接收READ，回复ANSR，发送SEND，响应RECV
    */
    UART_SEND_BOOT_DONE = -1,  // ESP32启动完成
    UART_READ_BLE_SET,         // 蓝牙设置
    UART_ANSR_BLE_SET,         // 蓝牙设置应答
    UART_SEND_BLE_STATE,       // 蓝牙连接状态
    UART_RECV_BLE_STATE,       // 蓝牙连接状态应答
    UART_READ_WIFI_INIT,       // WIFI开启
    UART_ANSR_WIFI_INIT,       // WIFI开启应答
    UART_READ_WIFI_SET,        // WIFI设置
    UART_ANSR_WIFI_SET,        // WIFI设置应答
    UART_READ_DHCP_START,      // DHCP开启
    UART_ANSR_DHCP_START,      // DHCP开启应答
    UART_READ_WIFI_STATIC_IP,  // 静态IP设置
    UART_ANSR_WIFI_STATIC_IP,  // 静态IP设置应答
    UART_READ_WIFI_CTRL,       // WIFI控制
    UART_ANSR_WIFI_CTRL,       // WIFI控制应答
    UART_SEND_WIFI_STATE,      // WIFI连接状态
    UART_RECV_WIFI_STATE,      // WIFI连接状态应答
    UART_READ_SERVER_SET,      // 设置服务器地址
    UART_ANSR_SERVER_SET,      // 设置服务器地址应答
    UART_SEND_SERVER_STATE,    // 服务器连接状态
    UART_RECV_SERVER_STATE,    // 服务器连接状态应答
    UART_SEND_DATA_READ,       // 串口接收数据向外部发送
    UART_ANSR_DATA_SEND,       // 串口发送数据应答
    UART_SEND_DATA_RECV,       // 外部接收数据向串口发送
    UART_RECV_DATA_ANSR,       // 串口接收数据应答
    UART_READ_WIFI_STATE,      // 查询WIFI连接状态
    UART_ANSR_WIFI_STATE,      // 查询WIFI连接状态回复
    UART_READ_OTA_INIT,        // OTA初始化
    UART_ANSR_OTA_INIT,        // OTA初始化应答
    UART_READ_OTA_DATA,        // OTA数据
    UART_ANSR_OTA_DATA,        // OTA数据应答
    UART_READ_OTA_DONE,        // OTA完成
    UART_ANSR_OTA_DONE,        // OTA完成应答
    UART_BLE_TEST_OPEN = 0xAA, // 蓝牙测试打开蓝牙
    UART_BLE_TEST_SEND = 0xBB, // 蓝牙测试发送数据
    UART_BLE_TEST_RECV = 0xCC, // 蓝牙测试接收数据
    UART_TEST          = 0xDD, // UART测试接收相同
};

typedef struct __attribute__((__packed__)) server_info
{
    uint8_t tls;
    uint16_t port;
    char *addr;
} server_info;

typedef struct __attribute__((__packed__)) uart_send
{
    uint16_t head;
    uint16_t size;
    uint8_t cmd;
    uint8_t flag;
    char data[0];
} uart_send;

typedef struct __attribute__((__packed__)) send_buffer
{
    uart_send *addr;
    uint16_t len;
} send_buffer;

/// @brief UART初始化
void esp32_uart_start(void);

/// @brief 串口发送数据，调用时会依次向发送队列写入待发送的数据，请注意队列容量
/// @param flag 命令标志
/// @param mode 单字节数据
/// @param data 数据指针，可为 NULL
/// @param len 数据长度，数据指针为 NULL 时长度需为0
/// @return 成功返回0，失败返回-1
int esp32_uart_send_data(uint8_t cmd, uint8_t flag, char *data, int len);
