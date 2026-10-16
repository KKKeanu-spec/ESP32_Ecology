/*!
 * @file net_report.h
 * @brief 上行上报与下行命令处理（把 BLE 侧的业务逻辑复用到 WiFi 通道）
 *
 * 上报内容:
 *   - 心跳/状态  每 NET_HB_INTERVAL_MS 一次
 *   - 状态事件   WiFi 上下线、推理启停
 *   - TinyML结果 每 NET_ML_REPORT_DIV 次推理上报一条（可开关）
 *
 * 下行命令（JSON）:
 *   {"c":"query_status"}
 *   {"c":"ml_start"} / {"c":"ml_stop"}
 *   {"c":"ml_run","x":0.25}
 *   {"c":"ml_report","on":1}
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/// 心跳间隔(ms)
#ifndef NET_HB_INTERVAL_MS
#define NET_HB_INTERVAL_MS (10000)
#endif

/// 每 N 次推理上报 1 条（降低数据量，1=每次都报）
#ifndef NET_ML_REPORT_DIV
#define NET_ML_REPORT_DIV (1)
#endif

/// @brief 初始化上报模块（注册 TCP 下行回调、挂接 TinyML 结果回调）
void net_report_init(void);

/// @brief 主动上报一条事件，如 "wifi_up" / "ml_start"
void net_report_event(const char *event);

/// @brief 开关 TinyML 结果上报
void net_report_set_ml(bool on);
bool net_report_get_ml(void);

#ifdef __cplusplus
}
#endif
