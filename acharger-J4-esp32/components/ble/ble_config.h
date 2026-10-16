/*!
 * @file ble_config.h
 * @brief BLE 参数集中配置（主机模式：不再由 CCU 下发，直接写死在代码里）
 *
 * 参数来源: 《交流桩与APP通信协议 V1.4》
 *   - 产品UUID表 第9条: J4 / 00010108 / RACE 22 / 欧标22kW
 *   - APP UUID表 第2条: 国际版 / 02000000
 *
 * 完整 128 位 UUID 形式（后 96 位为 SIG 标准基址，广播中省略）:
 *   产品: 00010108-0000-1000-8000-00805F9B34FB
 *   APP : 02000000-0000-1000-8000-00805F9B34FB
 *
 * 修改设备型号时，只需改这里的宏，无需改动其他代码。
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* 广播参数                                                                    */
/* -------------------------------------------------------------------------- */

/// 产品(硬件) UUID 前 32 位 —— 决定手机能否识别本设备型号
#ifndef BLE_HW_UUID
#define BLE_HW_UUID (0x00010108U)
#endif

/// APP UUID 前 32 位 —— 决定与哪个 APP 配对
#ifndef BLE_APP_UUID
#define BLE_APP_UUID (0x02000000U)
#endif

/// 广播设备名（协议表中为 "RACE 22"，含空格，注意原样保留）
#ifndef BLE_NAME
#define BLE_NAME "RACE 22"
#endif

/* -------------------------------------------------------------------------- */
/* 应用层协议信息（用于 "0x62 查询硬件信息应答"，不参与广播）                   */
/* -------------------------------------------------------------------------- */

/// 协议类型: 0x11 = 17
#ifndef BLE_PROTOCOL_TYPE
#define BLE_PROTOCOL_TYPE (0x11U)
#endif

/// 协议版本 1.6 -> 编码为 2 字节 [主版本, 次版本]
#ifndef BLE_PROTOCOL_VER_MAJOR
#define BLE_PROTOCOL_VER_MAJOR (0x01U)
#endif
#ifndef BLE_PROTOCOL_VER_MINOR
#define BLE_PROTOCOL_VER_MINOR (0x06U)
#endif

/* -------------------------------------------------------------------------- */
/* 广播扫描响应(Scan Response)自定义数据                                        */
/* -------------------------------------------------------------------------- */
/* BLE 广播包只有 31 字节，产品+APP 两个 32 位 UUID 加名称后已接近上限，
 * 因此把协议版本等附加信息放在 Scan Response 里（同样是厂商自定义数据段）。
 * 格式(小端): [协议类型 1B][主版本 1B][次版本 1B] */

#ifndef BLE_SCAN_RSP_DATA
#define BLE_SCAN_RSP_DATA {BLE_PROTOCOL_TYPE, BLE_PROTOCOL_VER_MAJOR, BLE_PROTOCOL_VER_MINOR}
#endif
#define BLE_SCAN_RSP_LEN (3)

#ifdef __cplusplus
}
#endif
