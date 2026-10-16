# -*- coding: utf-8 -*-
"""
TinyML 正弦波模型 —— 纯 numpy 训练脚本（无需 TensorFlow）

网络结构（与原 TensorFlow 官方 TinyML "hello world" 教程一致）:
    Input(1) -> Dense(16, relu) -> Dense(16, relu) -> Dense(1, linear)

目标函数: y = sin(2*pi*x),  x in [0, 1)

训练方式: 手写反向传播 + 全批量梯度下降(Momentum)，参数量约 337 个 float。

输出: components/tinyml/sine_weights.h
      —— 内含已量化好的 int8 权重与 float 权重两份，供 MCU 侧直接 #include。

用法:
    python tools/train_sine.py
"""

import os
import sys
import math
import numpy as np

# ----------------------------------------------------------------------------
# 超参数
# ----------------------------------------------------------------------------
HIDDEN_1 = 16
HIDDEN_2 = 16
EPOCHS = 40000
LR = 0.05
MOMENTUM = 0.9
SEED = 42

# 余弦退火: lr(t) = LR_MIN + 0.5*(LR-LR_MIN)*(1+cos(pi*t/T))
LR_MIN = 0.0005
LR_DECAY = True

# 输出路径：<repo>/components/tinyml/sine_weights.h
THIS_DIR = os.path.dirname(os.path.abspath(__file__))
OUT_DIR = os.path.join(THIS_DIR, "..", "acharger-J4-esp32", "components", "tinyml")
OUT_HEADER = os.path.join(OUT_DIR, "sine_weights.h")


# ----------------------------------------------------------------------------
# 生成数据: x = 均匀采样; y = sin(2*pi*x)
# ----------------------------------------------------------------------------
def make_dataset(n=1000):
    x = np.linspace(0.0, 1.0, n, endpoint=False, dtype=np.float64)
    y = np.sin(2.0 * np.pi * x)
    return x.reshape(-1, 1), y.reshape(-1, 1)


# ----------------------------------------------------------------------------
# 激活函数: tanh
#   单输入(x 为标量)的网络用 ReLU 极易落入全零死区——因为 16 个隐藏单元
#   的输入都是同一个 x 乘以权重，x≈0 时梯度为 0，神经元永久失活。
#   tanh 处处可导且有界，对 1 维输入回归是最稳的选择。
# ----------------------------------------------------------------------------
USE_TANH = True


def act(x):
    if USE_TANH:
        return np.tanh(x)
    return np.maximum(x, 0.0)


def act_grad(z):
    if USE_TANH:
        return 1.0 - np.tanh(z) ** 2
    return (z > 0.0).astype(z.dtype)


# ----------------------------------------------------------------------------
# 参数初始化 (He 初始化，适配 ReLU)
# ----------------------------------------------------------------------------
def he_normal(shape, rng, fan_in):
    std = math.sqrt(2.0 / fan_in)
    return rng.normal(0.0, std, size=shape)


def init_params(rng):
    return {
        "W1": he_normal((1, HIDDEN_1), rng, 1),
        "b1": np.zeros((1, HIDDEN_1)),
        "W2": he_normal((HIDDEN_1, HIDDEN_2), rng, HIDDEN_1),
        "b2": np.zeros((1, HIDDEN_2)),
        "W3": he_normal((HIDDEN_2, 1), rng, HIDDEN_2),
        "b3": np.zeros((1, 1)),
    }


# ----------------------------------------------------------------------------
# 前向传播
# ----------------------------------------------------------------------------
def forward(p, x):
    z1 = x @ p["W1"] + p["b1"]
    a1 = act(z1)
    z2 = a1 @ p["W2"] + p["b2"]
    a2 = act(z2)
    y = a2 @ p["W3"] + p["b3"]
    return z1, a1, z2, a2, y


# ----------------------------------------------------------------------------
# 训练: 全批量梯度下降 + Momentum
# ----------------------------------------------------------------------------
def train(x, y):
    rng = np.random.default_rng(SEED)
    p = init_params(rng)
    v = {k: np.zeros_like(val) for k, val in p.items()}

    n = x.shape[0]
    print("开始训练 ...")
    best = None
    for epoch in range(EPOCHS + 1):
        # 余弦退火学习率
        if LR_DECAY:
            lr = LR_MIN + 0.5 * (LR - LR_MIN) * (1.0 + math.cos(math.pi * epoch / EPOCHS))
        else:
            lr = LR
        z1, a1, z2, a2, y_hat = forward(p, x)

        # MSE loss
        diff = y_hat - y
        loss = float(np.mean(diff ** 2))

        # 反向传播
        d_y = 2.0 * diff / n                      # dL/dy_hat
        dW3 = a2.T @ d_y
        db3 = np.sum(d_y, axis=0, keepdims=True)

        d_a2 = d_y @ p["W3"].T
        d_z2 = d_a2 * act_grad(z2)
        dW2 = a1.T @ d_z2
        db2 = np.sum(d_z2, axis=0, keepdims=True)

        d_a1 = d_z2 @ p["W2"].T
        d_z1 = d_a1 * act_grad(z1)
        dW1 = x.T @ d_z1
        db1 = np.sum(d_z1, axis=0, keepdims=True)

        grads = {"W1": dW1, "b1": db1, "W2": dW2, "b2": db2, "W3": dW3, "b3": db3}

        # Momentum 更新
        for k in p:
            v[k] = MOMENTUM * v[k] - lr * grads[k]
            p[k] = p[k] + v[k]

        # 记录最优（防止退火末尾抖动）
        if best is None or loss < best[0]:
            best = (loss, {k: val.copy() for k, val in p.items()})

        if epoch % 2000 == 0:
            print("  epoch %6d  lr=%.5f  loss=%.8f" % (epoch, lr, loss))

    # 使用训练过程中的最优参数
    p = best[1]
    _, _, _, _, y_hat = forward(p, x)
    final_loss = float(np.mean((y_hat - y) ** 2))
    max_err = float(np.max(np.abs(y_hat - y)))
    print("训练完成: MSE=%.8f  MaxAbsErr=%.6f" % (final_loss, max_err))
    return p, final_loss, max_err


# ----------------------------------------------------------------------------
# 量化: float -> int8 (对称量化, scale = max|x| / 127)
# ----------------------------------------------------------------------------
def quantize_symmetric(arr):
    amax = float(np.max(np.abs(arr)))
    if amax < 1e-12:
        scale = 1.0
    else:
        scale = amax / 127.0
    q = np.round(arr / scale).astype(np.int32)
    q = np.clip(q, -127, 127).astype(np.int8)
    return q, scale


def format_array_c(name, arr_1d, ctype, per_line=8):
    """把一维数组格式化成 C 数组字面量"""
    lines = []
    vals = []
    for v in arr_1d:
        if ctype == "int8_t":
            vals.append("%4d" % int(v))
        else:
            vals.append("%+.9ff" % float(v))
    for i in range(0, len(vals), per_line):
        chunk = ", ".join(vals[i:i + per_line])
        lines.append("    " + chunk + ",")
    body = "\n".join(lines)
    return "static const %s %s[%d] = {\n%s\n};\n" % (ctype, name, len(vals), body)


# ----------------------------------------------------------------------------
# 导出 C 头文件
# ----------------------------------------------------------------------------
HEADER_TEMPLATE = """/*!
 * @file sine_weights.h
 * @brief 正弦波 TinyML 模型权重（由 tools/train_sine.py 自动生成，请勿手工修改）
 *
 * 网络结构: Input(1) -> Dense({h1}, relu) -> Dense({h2}, relu) -> Dense(1, linear)
 * 目标函数: y = sin(2*pi*x)
 *
 * 训练指标: MSE = {loss:.8f}, MaxAbsErr = {maxerr:.6f}
 *
 * 两种权重同时导出:
 *   1) FLOAT 版: sine_W1_f32 ...  直接用于纯 float 推理
 *   2) INT8  版: sine_W1_i8  ...  配合 per-tensor scale 做定点推理
 *
 * 生成时间: {ts}
 */

#pragma once

#include <stdint.h>

#define SINE_IN_DIM   1
#define SINE_H1_DIM   {h1}
#define SINE_H2_DIM   {h2}
#define SINE_OUT_DIM  1

/* ---------------- 模型元信息 ---------------- */
#define SINE_TRAIN_MSE      ({loss:.8f}f)
#define SINE_TRAIN_MAXERR   ({maxerr:.6f}f)

/* ---------------- Float 权重 ---------------- */
{float_arrays}
/* ---------------- Int8 量化权重 ---------------- */
/* 反量化: real = q * scale */
{int8_arrays}
/* ---------------- Int8 量化 scale ---------------- */
{scale_defs}
"""


def main():
    x, y = make_dataset()
    p, loss, max_err = train(x, y)

    # 量化
    qW1, sW1 = quantize_symmetric(p["W1"])
    qb1, sb1 = quantize_symmetric(p["b1"])
    qW2, sW2 = quantize_symmetric(p["W2"])
    qb2, sb2 = quantize_symmetric(p["b2"])
    qW3, sW3 = quantize_symmetric(p["W3"])
    qb3, sb3 = quantize_symmetric(p["b3"])

    float_arrays = "\n".join([
        format_array_c("sine_W1_f32", p["W1"].reshape(-1), "float"),
        format_array_c("sine_b1_f32", p["b1"].reshape(-1), "float"),
        format_array_c("sine_W2_f32", p["W2"].reshape(-1), "float"),
        format_array_c("sine_b2_f32", p["b2"].reshape(-1), "float"),
        format_array_c("sine_W3_f32", p["W3"].reshape(-1), "float"),
        format_array_c("sine_b3_f32", p["b3"].reshape(-1), "float"),
    ])

    int8_arrays = "\n".join([
        format_array_c("sine_W1_i8", qW1.reshape(-1), "int8_t"),
        format_array_c("sine_b1_i8", qb1.reshape(-1), "int8_t"),
        format_array_c("sine_W2_i8", qW2.reshape(-1), "int8_t"),
        format_array_c("sine_b2_i8", qb2.reshape(-1), "int8_t"),
        format_array_c("sine_W3_i8", qW3.reshape(-1), "int8_t"),
        format_array_c("sine_b3_i8", qb3.reshape(-1), "int8_t"),
    ])

    scale_defs = "\n".join([
        "#define SINE_SCALE_W1  (%.10ff)" % sW1,
        "#define SINE_SCALE_B1  (%.10ff)" % sb1,
        "#define SINE_SCALE_W2  (%.10ff)" % sW2,
        "#define SINE_SCALE_B2  (%.10ff)" % sb2,
        "#define SINE_SCALE_W3  (%.10ff)" % sW3,
        "#define SINE_SCALE_B3  (%.10ff)" % sb3,
    ])

    from datetime import datetime
    content = HEADER_TEMPLATE.format(
        h1=HIDDEN_1, h2=HIDDEN_2,
        loss=loss, maxerr=max_err,
        float_arrays=float_arrays,
        int8_arrays=int8_arrays,
        scale_defs=scale_defs,
        ts=datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
    )

    os.makedirs(OUT_DIR, exist_ok=True)
    with open(OUT_HEADER, "w", encoding="utf-8", newline="\n") as f:
        f.write(content)

    print("已生成: %s" % os.path.normpath(OUT_HEADER))
    print("参数量: W1=%d b1=%d W2=%d b2=%d W3=%d b3=%d" %
          (p["W1"].size, p["b1"].size, p["W2"].size, p["b2"].size, p["W3"].size, p["b3"].size))

    # 抽样验证
    print("\n抽样验证 (x -> predict / truth):")
    for xv in [0.0, 0.125, 0.25, 0.5, 0.75, 1.0]:
        _, _, _, _, yv = forward(p, np.array([[xv]]))
        print("  x=%.3f  pred=%+.5f  truth=%+.5f" % (xv, yv[0, 0], math.sin(2 * math.pi * xv)))


if __name__ == "__main__":
    main()
