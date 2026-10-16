/*!
 * @file net_client.h
 * @brief ESP32 作为 TCP 客户端连接远端服务器（WiFi 通道）
 *
 * 职责:
 *   1) 按配置的 地址:端口 建立并维持 TCP 连接（断线自动重连）
 *   2) 发送上行 JSON 行（以 '\n' 结尾）
 *   3) 接收下行 JSON 行并交给回调处理
 *
 * 协议（JSON Lines，一行一个 JSON，'\n' 分隔）:
 *   上行:
 *     {"t":"hb","ts":3600,"wifi":1,"rssi":-73,"heap":53800}      心跳/状态
 *     {"t":"evt","e":"wifi_up"|"wifi_down"|"ml_start"|"ml_stop"}  事件
 *     {"t":"ml","n":42,"x":0.4062,"pred":0.5556,"err":0.0000}     推理结果
 *     {"t":"ack","id":7,"ok":1}                                   指令应答
 *   下行:
 *     {"c":"query_status"}
 *     {"c":"ml_start"|"ml_stop"}
 *     {"c":"ml_run","x":0.25}
 *     {"c":"ml_report","on":1}
 *
 * 服务器地址由 BLE 下发并存入 NVS（见 net_config_* 接口）。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------- */
/* 配置持久化（NVS）                                                            */
/* -------------------------------------------------------------------------- */
#define NET_CFG_NVS_NAMESPACE "netcfg"
#define NET_CFG_KEY_HOST      "host"
#define NET_CFG_KEY_PORT      "port"
#define NET_CFG_KEY_ENABLE    "enable"

#define NET_HOST_MAX_LEN (64)

/// @brief 从 NVS 载入服务器配置（开机调用）
/// @return 成功返回 0；无配置返回 -1（此时不启用连接）
int net_config_load(void);

/// @brief 保存服务器配置到 NVS 并立即生效（BLE 下发时调用）
/// @param host 服务器地址（域名或 IP）
/// @param port 端口
/// @return 成功返回 0
int net_config_save(const char *host, uint16_t port);

/// @brief 读取当前配置（供上报/查询使用）
/// @param host 输出缓冲（至少 NET_HOST_MAX_LEN 字节），可为 NULL
/// @param port 输出端口，可为 NULL
/// @return 已配置返回 0；未配置返回 -1
int net_config_get(char *host, uint16_t *port);

/// @brief 是否已启用云端连接（有配置且已开启）
bool net_is_enabled(void);

/* -------------------------------------------------------------------------- */
/* 连接管理                                                                    */
/* -------------------------------------------------------------------------- */

/// @brief 启动 TCP 客户端任务（内部会按需连接/重连）
void net_client_start(void);

/// @brief 查询当前是否已连接到服务器
bool net_is_connected(void);

/// @brief 发送一行上行数据（自动补 '\n'）。内部有发送队列，非阻塞。
/// @param json 不含换行的 JSON 字符串
/// @return 入队成功返回 0，失败返回 -1（未连接/队列满/超长）
int net_send_json(const char *json);

/// @brief 格式化并发送（内部用 vsnprintf，便于拼装）
/// @return 同 net_send_json
int net_send_jsonf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/// @brief 注册下行数据回调（收到一行 JSON 时触发，data 已去掉换行）
///        回调在 net 任务上下文中执行，请勿阻塞。
void net_set_rx_callback(void (*cb)(const char *line));

#ifdef __cplusplus
}
#endif
