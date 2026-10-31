/*!
 * @file tinyml.c
 * @brief TinyML 周期推理任务实现（支持 BLE 远程启停）
 */

#include "tinyml.h"
#include "tiny_sine.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"

/* 仅在需要把推理结果发给 GD32/CCU 时引入 UART 协议头。
 * 主机模式下 TINYML_SEND_VIA_UART = 0，此处不参与编译；
 * 后续若需恢复对外发送，把 tinyml.h 中该宏置 1 即可，无需改动其他代码。 */
#if TINYML_SEND_VIA_UART
#include "esp32_uart.h"
#endif

#include <math.h>
#include <string.h>

static const char *TAG = "TinyML-App";

/* -------------------------------------------------------------------------- */
/* 任务句柄与运行状态                                                          */
/* -------------------------------------------------------------------------- */
static TaskHandle_t s_task     = NULL;
static volatile bool s_running = false;

/* 恢复任务时置位，通知任务循环重置 vTaskDelayUntil 的时间基准，
 * 避免挂起期间的"追赶式"连续执行。 */
static volatile bool s_reset_wake = false;

/* 结果订阅者：支持多个上层模块同时订阅推理结果。
 * BLE 通道(ble_app) 与 WiFi 通道(net_report) 可各自注册一个回调，
 * 避免互相覆盖。用回调而非直接调用，避免 tinyml 反向依赖其他组件。 */
#define TINYML_MAX_SUBSCRIBERS (4)
static void (*s_result_cbs[TINYML_MAX_SUBSCRIBERS])(float phase, float pred) = {NULL};

void tinyml_set_result_callback(void (*cb)(float phase, float pred))
{
    /* 兼容旧接口：占用第一个槽位（BLE 使用） */
    s_result_cbs[0] = cb;
}

void tinyml_add_result_callback(void (*cb)(float phase, float pred))
{
    if (cb == NULL) return;

    for (int i = 0; i < TINYML_MAX_SUBSCRIBERS; i++)
    {
        if (s_result_cbs[i] == cb) return; /* 已注册 */
    }
    for (int i = 0; i < TINYML_MAX_SUBSCRIBERS; i++)
    {
        if (s_result_cbs[i] == NULL)
        {
            s_result_cbs[i] = cb;
            return;
        }
    }
    ESP_LOGW(TAG, "结果订阅者已满(%d)，本次注册被忽略", TINYML_MAX_SUBSCRIBERS);
}

/* -------------------------------------------------------------------------- */
/* 相位发生器：每拍推进 1/32 周期，形成连续波形便于观察                         */
/* -------------------------------------------------------------------------- */
#define PHASE_STEPS (32)
static uint32_t s_step = 0;

float tinyml_run_once(float x)
{
    /* 输入归一化到 [0,1) */
    x = x - floorf(x);

    float pred  = tiny_sine_predict(x);
    float truth = sinf(2.0f * (float)M_PI * x);

    ESP_LOGI(TAG, "infer  x=%.4f  pred=%+.4f  truth=%+.4f  err=%.4f",
             x, pred, truth, fabsf(pred - truth));

    /* 通过回调把结果推给所有订阅者（BLE / WiFi 通道等） */
    for (int i = 0; i < TINYML_MAX_SUBSCRIBERS; i++)
    {
        if (s_result_cbs[i])
        {
            s_result_cbs[i](x, pred);
        }
    }

    /* 兼容旧路径: 需要时可恢复向 GD32/CCU 发送 */
#if TINYML_SEND_VIA_UART
    struct __attribute__((__packed__))
    {
        float phase;
        float value;
    } payload = {.phase = x, .value = pred};

    esp32_uart_send_data(UART_SEND_DATA_RECV, NET_DATA, (char *)&payload, sizeof(payload));
#endif

    return pred;
}

/* -------------------------------------------------------------------------- */
/* 周期任务                                                                    */
/* -------------------------------------------------------------------------- */
static void tinyml_task(void *arg)
{
    ESP_LOGI(TAG, "tinyml task created, period = %d ms", TINYML_PERIOD_MS);

    /* 启动后先让出 CPU 一段时间。
     * 原因: BLE + WiFi 共存时，WiFi 扫描/认证阶段对时序敏感；
     * 若此时本任务立刻做密集软件浮点运算(tanhf 无 FPU)，会与之抢占 CPU，
     * 表现为 WiFi 认证阶段随机复位。此处等待 WiFi 稳定后再开始工作。 */
    vTaskDelay(pdMS_TO_TICKS(TINYML_START_DELAY_MS));

    tiny_sine_info();
    tiny_sine_selftest(8, true);

    const TickType_t period = pdMS_TO_TICKS(TINYML_PERIOD_MS);
    TickType_t last_wake    = xTaskGetTickCount();

    for (;;)
    {
        /* 被 vTaskResume 恢复时重置时间基准。
         * vTaskDelayUntil 用的是绝对时间：若任务被挂起较久，
         * 恢复后 last_wake 已严重落后当前时刻，会连续"追赶"执行
         * （表现为短时间爆发式推理），因此必须重置。 */
        if (s_reset_wake)
        {
            s_reset_wake = false;
            last_wake    = xTaskGetTickCount();
        }

        float phase = (float)s_step / (float)PHASE_STEPS;
        s_step      = (s_step + 1) % PHASE_STEPS;

        tinyml_run_once(phase);

        /* 用绝对时间基准延时，避免推理耗时累积成周期漂移 */
        vTaskDelayUntil(&last_wake, period);
    }
}

void tinyml_task_start(void)
{
    if (s_task != NULL)
    {
        /* 任务已存在，直接确保处于运行态 */
        tinyml_start();
        return;
    }

    s_running = true;
    xTaskCreate(tinyml_task, "tinyml", 1024 * 4, NULL, TINYML_TASK_PRIO, &s_task);
    ESP_LOGI(TAG, "tinyml periodic task started (prio=%d, start_delay=%dms)",
             TINYML_TASK_PRIO, TINYML_START_DELAY_MS);
}

void tinyml_start(void)
{
    if (s_task == NULL)
    {
        tinyml_task_start();
        return;
    }
    if (s_running)
    {
        ESP_LOGI(TAG, "tinyml already running");
        return;
    }

    /* 先置位再恢复：任务一旦被唤醒即会重置时间基准 */
    s_reset_wake = true;
    vTaskResume(s_task);
    s_running = true;
    ESP_LOGI(TAG, "tinyml resumed (running)");
}

void tinyml_stop(void)
{
    if (s_task == NULL || !s_running)
    {
        ESP_LOGI(TAG, "tinyml already stopped");
        return;
    }
    vTaskSuspend(s_task);
    s_running = false;
    ESP_LOGI(TAG, "tinyml suspended (stopped)");
}

bool tinyml_is_running(void)
{
    return s_running;
}
