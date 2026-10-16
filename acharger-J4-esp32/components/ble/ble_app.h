/*!
 * @file ble_app.h
 * @brief BLE 应用层命令分发（主机模式：ESP32 自己处理 APP 下发的数据）
 *
 * 取代原「BLE 收到 -> 转发给 GD32/CCU」的逻辑。
 * 数据从 0xABF1(数据) / 0xABF3(命令) 写入，本模块统一解析。
 *
 * 协议（第一字节为命令码）:
 *   0x59  [网络类型][IP方式][SSID长度][密码长度][SSID][密码...]
 *         设置网络通信方式（WiFi 配置）
 *   0x61  查询硬件信息
 *   0xE0  停止 TinyML 周期推理
 *   0xE1  启动 TinyML 周期推理
 *   0xE2  [float x, 小端]  单次推理
 *
 * 应答（从 0xABF2 通知回传）:
 *   0x5A  [收到配置][网络状态][MQTT状态]   WiFi 配置应答
 *   0x62  [协议类型][版本2B][硬件名16B][硬件版本3B][软件名16B][软件版本3B]
 *   0xE0  [结果]   停止应答  0x01=成功 0x00=失败
 *   0xE1  [结果]   启动应答
 *   0xE2  [float y, 小端]  单次推理结果
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* 命令码                                                                      */
/* -------------------------------------------------------------------------- */
#define BLE_CMD_NET_SET      (0x59) /* APP -> ESP32: 设置网络通信方式 */
#define BLE_CMD_NET_ACK      (0x5A) /* ESP32 -> APP: 应答 */
#define BLE_CMD_HW_QUERY     (0x61) /* APP -> ESP32: 查询硬件信息 */
#define BLE_CMD_HW_ACK       (0x62) /* ESP32 -> APP: 应答 */
#define BLE_CMD_TINYML_STOP  (0xE0) /* APP -> ESP32: 停止周期推理 */
#define BLE_CMD_TINYML_START (0xE1) /* APP -> ESP32: 启动周期推理 */
#define BLE_CMD_TINYML_RUN   (0xE2) /* APP -> ESP32: 单次推理 [float x] */
#define BLE_CMD_NET_SRV_SET  (0xE4) /* APP -> ESP32: 下发云端服务器地址
                                     *   载荷: [portLo][portHi][host\0]
                                     *   小端端口 + 以 '\0' 结尾的主机名
                                     *   应答: [0xE4][ok][portLo][portHi] */
#define BLE_CMD_NET_SRV_ACK  (0xE4) /* ESP32 -> APP: 应答 */

/* 应答状态码 */
#define BLE_ACK_OK   (0x01)
#define BLE_ACK_FAIL (0x00)

/* -------------------------------------------------------------------------- */
/* 硬件/软件信息（用于 0x62 应答，按需修改）                                     */
/* -------------------------------------------------------------------------- */
#ifndef BLE_HW_NAME
#define BLE_HW_NAME "ESP32-C3"
#endif
#ifndef BLE_HW_VER_MAJOR
#define BLE_HW_VER_MAJOR (1)
#endif
#ifndef BLE_HW_VER_MINOR
#define BLE_HW_VER_MINOR (0)
#endif
#ifndef BLE_HW_VER_PATCH
#define BLE_HW_VER_PATCH (0)
#endif

#ifndef BLE_SW_NAME
#define BLE_SW_NAME "TINYML"
#endif
#ifndef BLE_SW_VER_MAJOR
#define BLE_SW_VER_MAJOR (1)
#endif
#ifndef BLE_SW_VER_MINOR
#define BLE_SW_VER_MINOR (0)
#endif
#ifndef BLE_SW_VER_PATCH
#define BLE_SW_VER_PATCH (0)
#endif

/* -------------------------------------------------------------------------- */
/* 接口                                                                        */
/* -------------------------------------------------------------------------- */

/// @brief 初始化 BLE 应用层（注册 TinyML 结果回调、初始化 WiFi、启动状态上报任务）
void ble_app_init(void);

/// @brief 处理来自 BLE 的一帧数据（在 GATT 写回调里调用）
/// @param data 数据指针
/// @param len  数据长度
void ble_app_handle_data(const uint8_t *data, uint16_t len);

/// @brief 是否把推理结果推送到 BLE（控制是否推送，避免无谓的 notify）
void ble_app_set_tinyml_report(bool on);

#ifdef __cplusplus
}
#endif
