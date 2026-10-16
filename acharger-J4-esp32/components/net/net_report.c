/*!
 * @file net_report.c
 * @brief 上行上报（心跳/事件/TinyML结果）与下行命令处理
 */

#include "net_report.h"
#include "net_client.h"
#include "tinyml.h"

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

static const char *TAG = "NET-RPT";

static volatile bool s_ml_report = true;   /* 是否上报 TinyML 结果 */
static uint32_t      s_ml_seq    = 0;      /* 上报序号 */

/* -------------------------------------------------------------------------- */
/* 极简 JSON 取值（不引入 cJSON，避免额外依赖与内存开销）                        */
/* -------------------------------------------------------------------------- */

/// 从 JSON 行中取字符串字段值，如 get_str(src,"c",out,sizeof(out)) 取 "c":"xxx"
static bool json_get_str(const char *src, const char *key, char *out, size_t out_len)
{
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);

    const char *p = strstr(src, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p != '"') return false;
    p++;

    size_t i = 0;
    while (*p && *p != '"' && i < out_len - 1)
    {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return true;
}

/// 从 JSON 行中取整数字段值
static bool json_get_num(const char *src, const char *key, double *out)
{
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);

    const char *p = strstr(src, pat);
    if (!p) return false;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;

    char *end = NULL;
    double v  = strtod(p, &end);
    if (end == p) return false;
    *out = v;
    return true;
}

/* -------------------------------------------------------------------------- */
/* 上行                                                                         */
/* -------------------------------------------------------------------------- */
void net_report_event(const char *event)
{
    if (event == NULL) return;
    net_send_jsonf("{\"t\":\"evt\",\"e\":\"%s\"}", event);
}

void net_report_set_ml(bool on)
{
    s_ml_report = on;
    ESP_LOGI(TAG, "TinyML 结果上报: %s", on ? "开" : "关");
}

bool net_report_get_ml(void) { return s_ml_report; }

/* -------------------------------------------------------------------------- */
/* TinyML 结果回调（周期推理每次触发）                                          */
/* -------------------------------------------------------------------------- */
static void on_ml_result(float phase, float pred)
{
    if (!s_ml_report) return;
    if (!net_is_connected()) return;

    /* 抽稀: 每 NET_ML_REPORT_DIV 条上报一条 */
    static uint32_t cnt = 0;
    cnt++;
    if (NET_ML_REPORT_DIV > 1 && (cnt % NET_ML_REPORT_DIV) != 0) return;

    float truth = sinf(2.0f * (float)M_PI * phase);
    float err   = fabsf(pred - truth);

    net_send_jsonf("{\"t\":\"ml\",\"n\":%lu,\"x\":%.4f,\"pred\":%.4f,\"err\":%.4f}",
                   (unsigned long)(++s_ml_seq), phase, pred, err);
}

/* -------------------------------------------------------------------------- */
/* 下行命令处理                                                                 */
/* -------------------------------------------------------------------------- */
static void handle_command(const char *line)
{
    char cmd[32] = {0};
    if (!json_get_str(line, "c", cmd, sizeof(cmd)))
    {
        ESP_LOGW(TAG, "下行缺少 \"c\" 字段: %s", line);
        return;
    }

    ESP_LOGI(TAG, "执行命令: %s", cmd);

    if (strcmp(cmd, "query_status") == 0)
    {
        char host[NET_HOST_MAX_LEN] = {0};
        uint16_t port = 0;
        net_config_get(host, &port);

        net_send_jsonf("{\"t\":\"status\",\"ml_run\":%d,\"ml_report\":%d,"
                       "\"heap\":%u,\"up\":%lld,\"srv\":\"%s:%u\"}",
                       tinyml_is_running() ? 1 : 0,
                       s_ml_report ? 1 : 0,
                       (unsigned)xPortGetFreeHeapSize(),
                       (long long)(esp_timer_get_time() / 1000000),
                       host, port);
    }
    else if (strcmp(cmd, "ml_start") == 0)
    {
        tinyml_start();
        net_send_jsonf("{\"t\":\"ack\",\"c\":\"ml_start\",\"ok\":1}");
        net_report_event("ml_start");
    }
    else if (strcmp(cmd, "ml_stop") == 0)
    {
        tinyml_stop();
        net_send_jsonf("{\"t\":\"ack\",\"c\":\"ml_stop\",\"ok\":1}");
        net_report_event("ml_stop");
    }
    else if (strcmp(cmd, "ml_run") == 0)
    {
        double x = 0.0;
        if (json_get_num(line, "x", &x))
        {
            float y = tinyml_run_once((float)x);
            net_send_jsonf("{\"t\":\"ml_once\",\"x\":%.4f,\"pred\":%.4f}", (float)x, y);
        }
        else
        {
            net_send_jsonf("{\"t\":\"ack\",\"c\":\"ml_run\",\"ok\":0,\"msg\":\"missing x\"}");
        }
    }
    else if (strcmp(cmd, "ml_report") == 0)
    {
        double on = 0;
        if (json_get_num(line, "on", &on))
        {
            net_report_set_ml(on != 0);
            net_send_jsonf("{\"t\":\"ack\",\"c\":\"ml_report\",\"ok\":1,\"on\":%d}", (int)(on != 0));
        }
    }
    else
    {
        ESP_LOGW(TAG, "未知命令: %s", cmd);
        net_send_jsonf("{\"t\":\"ack\",\"c\":\"%s\",\"ok\":0,\"msg\":\"unknown\"}", cmd);
    }
}

/* -------------------------------------------------------------------------- */
/* 心跳任务                                                                     */
/* -------------------------------------------------------------------------- */
static void hb_task(void *arg)
{
    bool last_conn = false;

    for (;;)
    {
        bool conn = net_is_connected();

        /* 连接状态变化时上报事件 */
        if (conn != last_conn)
        {
            last_conn = conn;
            ESP_LOGI(TAG, "云端连接状态: %s", conn ? "已连接" : "已断开");
            if (conn) net_report_event("net_up");
        }

        if (conn)
        {
            char host[NET_HOST_MAX_LEN] = {0};
            uint16_t port = 0;
            net_config_get(host, &port);

            net_send_jsonf("{\"t\":\"hb\",\"up\":%lld,\"heap\":%u,"
                           "\"ml_run\":%d,\"srv\":\"%s:%u\"}",
                           (long long)(esp_timer_get_time() / 1000000),
                           (unsigned)xPortGetFreeHeapSize(),
                           tinyml_is_running() ? 1 : 0,
                           host, port);
        }

        vTaskDelay(pdMS_TO_TICKS(NET_HB_INTERVAL_MS));
    }
}

/* -------------------------------------------------------------------------- */
/* 初始化                                                                       */
/* -------------------------------------------------------------------------- */
void net_report_init(void)
{
    /* 订阅 TinyML 结果（不覆盖 BLE 已注册的回调） */
    tinyml_add_result_callback(on_ml_result);

    /* 注册下行命令回调 */
    net_set_rx_callback(handle_command);

    /* 心跳/连接状态任务 */
    xTaskCreate(hb_task, "net_hb", 1024 * 3, NULL, 3, NULL);

    ESP_LOGI(TAG, "上报模块已初始化（心跳 %d ms, ML上报=%s）",
             NET_HB_INTERVAL_MS, s_ml_report ? "开" : "关");
}
