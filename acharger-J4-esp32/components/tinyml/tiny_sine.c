/*!
 * @file tiny_sine.c
 * @brief 正弦波 TinyML 推理（纯 C，无第三方依赖）
 *
 * 设计要点:
 *  - 不使用 ReLU: 单输入(1 维)网络用 ReLU 会出现"全零死区"
 *    (16 个隐藏单元共享同一个输入 x，x≈0 时梯度恒为 0，神经元永久失活)，
 *    因此训练与推理统一使用 tanh。
 *  - 前向计算全为标量乘加，参数常驻 Flash(const)，栈上只有中间激活，
 *    单次推理 RAM 需求 < 200 字节，无需 tensor arena。
 *  - ESP32-C3 无硬件浮点(tanhf 为软件实现)，但模型极小，
 *    实测单次推理约几十微秒量级，2 秒周期任务完全无压力。
 */

#include "tiny_sine.h"
#include "sine_weights.h"

#include <math.h>
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "TinyML";

/* -------------------------------------------------------------------------- */
/* 权重的只读视图（避免散落的宏，便于扩展为多模型）                            */
/* -------------------------------------------------------------------------- */
static const float *const W1 = sine_W1_f32; /* (1, 16)  */
static const float *const b1 = sine_b1_f32; /* (16)     */
static const float *const W2 = sine_W2_f32; /* (16, 16) */
static const float *const b2 = sine_b2_f32; /* (16)     */
static const float *const W3 = sine_W3_f32; /* (16, 1)  */
static const float *const b3 = sine_b3_f32; /* (1)      */

/* -------------------------------------------------------------------------- */
/* 前向传播                                                                    */
/* -------------------------------------------------------------------------- */
float tiny_sine_predict(float x)
{
    float h1[SINE_H1_DIM];
    float h2[SINE_H2_DIM];

    /* 第 1 层: y = tanh(x * W1 + b1) */
    for (int i = 0; i < SINE_H1_DIM; i++)
    {
        h1[i] = tanhf(x * W1[i] + b1[i]);
    }

    /* 第 2 层: y = tanh(h1 * W2 + b2) ，W2 按行优先存储 [16 x 16] */
    for (int j = 0; j < SINE_H2_DIM; j++)
    {
        float acc = b2[j];
        for (int i = 0; i < SINE_H1_DIM; i++)
        {
            acc += h1[i] * W2[i * SINE_H2_DIM + j];
        }
        h2[j] = tanhf(acc);
    }

    /* 第 3 层: 线性输出 */
    float out = b3[0];
    for (int j = 0; j < SINE_H2_DIM; j++)
    {
        out += h2[j] * W3[j];
    }

    return out;
}

/* -------------------------------------------------------------------------- */
/* 自检: 对比预测值与真实 sin，衡量部署后的实际精度                            */
/* -------------------------------------------------------------------------- */
float tiny_sine_selftest(int samples, bool print_each)
{
    if (samples <= 0)
    {
        samples = 8;
    }

    float max_err = 0.0f;

    for (int i = 0; i < samples; i++)
    {
        float x      = (float)i / (float)samples;
        float pred   = tiny_sine_predict(x);
        float truth  = sinf(2.0f * (float)M_PI * x);
        float err    = fabsf(pred - truth);

        if (err > max_err)
        {
            max_err = err;
        }

        if (print_each)
        {
            ESP_LOGI(TAG, "x=%.3f  pred=%+.4f  truth=%+.4f  err=%.4f", x, pred, truth, err);
        }
    }

    ESP_LOGI(TAG, "selftest: samples=%d  max_abs_err=%.5f", samples, max_err);
    return max_err;
}

/* -------------------------------------------------------------------------- */
/* 模型信息                                                                    */
/* -------------------------------------------------------------------------- */
void tiny_sine_info(void)
{
    const int n_params = SINE_H1_DIM        /* W1 */
                         + SINE_H1_DIM      /* b1 */
                         + SINE_H1_DIM * SINE_H2_DIM /* W2 */
                         + SINE_H2_DIM      /* b2 */
                         + SINE_H2_DIM      /* W3 */
                         + SINE_OUT_DIM;    /* b3 */

    ESP_LOGI(TAG, "model : Dense(1->%d,tanh) -> Dense(%d->%d,tanh) -> Dense(%d->1)",
             SINE_H1_DIM, SINE_H1_DIM, SINE_H2_DIM, SINE_H2_DIM);
    ESP_LOGI(TAG, "params: %d floats (%d bytes)", n_params, (int)(n_params * sizeof(float)));
    ESP_LOGI(TAG, "train : MSE=%.8f  MaxAbsErr=%.5f", SINE_TRAIN_MSE, SINE_TRAIN_MAXERR);

    /* 测一次推理耗时，验证实时性余量。
     * 次数不宜过多: 在 BLE+WiFi 共存场景下，长时间连续软件浮点运算(tanhf)
     * 会与射频任务抢 CPU，故只取 20 次做均值。 */
    int64_t t0  = esp_timer_get_time();
    float acc   = 0.0f;
    const int N = 20;
    for (int i = 0; i < N; i++)
    {
        acc += tiny_sine_predict((float)i / (float)N);
    }
    int64_t dt = esp_timer_get_time() - t0;

    ESP_LOGI(TAG, "infer : %lld us/次 (100 次共 %lld us, checksum=%.3f)",
             dt / N, dt, acc);
}
