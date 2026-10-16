/*!
 * @file tiny_sine.h
 * @brief 正弦波 TinyML 推理（纯 C 实现，无任何第三方依赖 / 不需要 TensorFlow）
 *
 * 模型: Input(1) -> Dense(16, tanh) -> Dense(16, tanh) -> Dense(1, linear)
 * 目标: 逼近 y = sin(2*pi*x),  x 属于 [0, 1)
 *
 * 权重由 tools/train_sine.py 训练并生成到 sine_weights.h，
 * 编译期直接编入固件（Flash 约 1.4KB），运行期 RAM 占用 < 200 字节。
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/// @brief 单次推理
/// @param x      输入标量，取值范围 [0.0, 1.0)，代表归一化相位
/// @return       预测的 sin(2*pi*x) 值，范围约 [-1, 1]
float tiny_sine_predict(float x);

/// @brief 打印模型基本信息（参数量、训练指标、单次推理耗时）
void tiny_sine_info(void);

/// @brief 自检：在 [0,1) 上均匀采样若干点，对比预测值与真实 sin 值
/// @param samples 采样点数，建议 8~16
/// @param print_each 是否逐点打印
/// @return 最大绝对误差
float tiny_sine_selftest(int samples, bool print_each);

#ifdef __cplusplus
}
#endif
