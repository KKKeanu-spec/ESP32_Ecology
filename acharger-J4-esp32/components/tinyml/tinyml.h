/*!
 * @file tinyml.h
 * @brief TinyML 应用层：周期性推理任务（可由 BLE 远程启停）
 *
 * 运行方式:
 *   1) 上电后自动启动周期任务（每 TINYML_PERIOD_MS 推进一步相位）
 *   2) 支持通过 BLE 指令停止/启动周期任务、或触发单次推理
 *
 * BLE 指令（走 0xABF1 数据通道，见 ble_app.h 的 TINYML_CMD_*）:
 *   0xE0               停止周期推理
 *   0xE1               启动周期推理
 *   0xE2 + float x     单次推理（x 为小端 float）
 * 结果统一从 0xABF2 通知回传。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief 推理周期(ms)，默认 5000
///        周期越短，BLE 推送与串口日志越频繁；5s 在演示效果与负载间较均衡
#ifndef TINYML_PERIOD_MS
#define TINYML_PERIOD_MS (5000)
#endif

/// @brief 允许的周期范围(ms)，防止被设成 0 或过小导致任务密集占用 CPU
#define TINYML_PERIOD_MIN_MS (200)
#define TINYML_PERIOD_MAX_MS (60000)

/// @brief 任务创建后延迟多久才开始工作(ms)
///        BLE+WiFi 共存时，等待 WiFi 扫描/认证完成，避免抢占 CPU 导致复位
#ifndef TINYML_START_DELAY_MS
#define TINYML_START_DELAY_MS (8000)
#endif

/// @brief TinyML 任务优先级（低于 WiFi 驱动、UART 任务，避免抢占）
#ifndef TINYML_TASK_PRIO
#define TINYML_TASK_PRIO (1)
#endif

/// @brief 是否把推理结果通过通信串口发给 GD32/CCU（1=发，0=仅本地打印）
///        主机模式下不再向 GD32 发送，默认关闭。
#ifndef TINYML_SEND_VIA_UART
#define TINYML_SEND_VIA_UART (0)
#endif

/// @brief 启动 TinyML 周期推理任务（创建任务，默认运行中）
void tinyml_task_start(void);

/// @brief 启动/恢复周期推理。任务未创建时会自动创建。
void tinyml_start(void);

/// @brief 停止周期推理（挂起任务，保留任务句柄以便恢复）
void tinyml_stop(void);

/// @brief 查询当前是否在运行
bool tinyml_is_running(void);

/// @brief 手动触发一次推理（供 BLE / UART 指令调用）
/// @param x 输入相位 [0,1)，越界会被归一到 [0,1)
/// @return 预测值
float tinyml_run_once(float x);

/// @brief 注册推理结果回调（用于把结果推给 BLE 客户端等上层）
///        每次推理后会调用 cb(phase, pred)。传 NULL 可取消注册。
///        注意: 此接口占用槽位 0，会覆盖上一次调用设置的回调。
void tinyml_set_result_callback(void (*cb)(float phase, float pred));

/// @brief 追加一个推理结果订阅者（不覆盖已有回调）
///        最多支持 4 个；BLE 与 WiFi 通道可各注册一个。
void tinyml_add_result_callback(void (*cb)(float phase, float pred));

#ifdef __cplusplus
}
#endif
