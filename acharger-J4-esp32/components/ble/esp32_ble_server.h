#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/// @brief BLE初始化
/// @return 成功返回0，失败返回-1
int esp32_ble_init();

/// @brief 使用代码内写死的参数启动 BLE（主机模式，不依赖 CCU 下发）
///        参数见 ble_config.h: BLE_HW_UUID / BLE_APP_UUID / BLE_NAME
/// @return 成功返回0，失败返回-1
int esp32_ble_start_with_config(void);

/// @brief 设置广播设备名
/// @param name 设备名指针
/// @param size 设备名长度
void esp32_ble_set_adv_name(char *name, uint8_t size);

/// @brief 设置广播UUID
/// @param uuid_1 UUID_1
/// @param uuid_2 UUID_2
void esp32_ble_set_adv_uuid(uint32_t uuid_1, uint32_t uuid_2);

/// @brief 设置响应数据
/// @param data 数据指针
/// @param size 数据长度
void esp32_ble_set_scan_rsp(char *data, uint8_t size);

/// @brief BLE数据
/// @param data 数据指针
/// @param size 数据长度
/// @return 成功返回0，失败返回-1
int esp32_gatt_send_data(void *data, uint16_t size);

/// @brief 查询当前是否有 BLE 中央设备已连接
/// @return true=已连接, false=未连接
bool esp32_ble_is_connected(void);

/// @brief BLE测试专用
int esp32_ble_test_mode();

#define SPP_DATA_MAX_LEN      (512)
#define SPP_CMD_MAX_LEN       (20)
#define SPP_STATUS_MAX_LEN    (20)
#define SPP_DATA_BUFF_MAX_LEN (2 * 1024)

enum
{
    SPP_IDX_SVC,

    SPP_IDX_SPP_DATA_RECV_CHAR,
    SPP_IDX_SPP_DATA_RECV_VAL,

    SPP_IDX_SPP_DATA_NOTIFY_CHAR,
    SPP_IDX_SPP_DATA_NTY_VAL,
    SPP_IDX_SPP_DATA_NTF_CFG,

    SPP_IDX_SPP_COMMAND_CHAR,
    SPP_IDX_SPP_COMMAND_VAL,

    SPP_IDX_SPP_STATUS_CHAR,
    SPP_IDX_SPP_STATUS_VAL,
    SPP_IDX_SPP_STATUS_CFG,

    SPP_IDX_NB,
};

enum
{
    BLE_ADV_FLAG = 0x01,              // BLE特征
    BLE_ADV_INCOMPLETE_16_UUID,       // 非完整16位UUID
    BLE_ADV_COMPLETE_16_UUID,         // 非完整16位UUID
    BLE_ADV_INCOMPLETE_32_UUID,       // 非完整32位UUID
    BLE_ADV_COMPLETE_32_UUID,         // 完整32位UUID
    BLE_ADV_INCOMPLETE_128_UUID,      // 非完整128位UUID
    BLE_ADV_COMPLETE_128_UUID,        // 完整128位UUID
    BLE_ADV_FULL_NAME,                // 蓝牙全名
    BLE_ADV_SHORT_NAME,               // 蓝牙简称
    BLE_ADV_TX_POWER,                 // 发射功率
    BLE_ADV_SERVICE_DATA      = 0x16, // 服务数据
    BLE_ADV_MANUFACTURER_DATA = 0xff, // 自定义数据
};

typedef struct __attribute__((__packed__)) ble_flag_t
{
    uint8_t size;
    uint8_t type;
    uint8_t flag;
} ble_flag_t;

typedef struct __attribute__((__packed__)) ble_uuid_t
{
    uint8_t size;
    uint8_t type;
    uint32_t uuid_1;
    uint32_t uuid_2;
} ble_uuid_t;

typedef struct __attribute__((__packed__)) ble_name_t
{
    uint8_t size;
    uint8_t type;
    uint8_t name[16];
} ble_name_t;

typedef struct __attribute__((__packed__)) ble_adv_data_t
{
    ble_flag_t flag;
    ble_uuid_t uuid;
    ble_name_t name;
} ble_adv_data_t;

typedef struct __attribute__((__packed__)) ble_data_t
{
    uint8_t size;
    uint8_t type;
    uint8_t data[29];
} ble_data_t;

typedef struct __attribute__((__packed__))
{
    ble_data_t data;
} ble_scan_rsp_t;