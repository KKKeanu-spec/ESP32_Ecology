/*!
 * @file net_client.c
 * @brief ESP32 TCP 客户端实现（连接远端服务器，JSON 行协议）
 */

#include "net_client.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "lwip/errno.h"

#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

static const char *TAG = "NET";

/* -------------------------------------------------------------------------- */
/* 配置                                                                         */
/* -------------------------------------------------------------------------- */
#define NET_TCP_RECONNECT_MS   (5000) /* 重连间隔 */
#define NET_TCP_CONN_TIMEOUT_S (8)    /* 连接超时 */
#define NET_TCP_RX_TIMEOUT_S   (2)    /* 接收超时(用于轮询发送队列) */
#define NET_SEND_QUEUE_LEN     (24)
#define NET_LINE_MAX_LEN       (512)

static char     s_host[NET_HOST_MAX_LEN] = {0};
static uint16_t s_port                   = 0;
static bool     s_enable                 = false;

static volatile bool s_connected = false;
static int           s_sock      = -1;

static QueueHandle_t s_tx_queue = NULL; /* 存放待发送字符串 */

static void (*s_rx_cb)(const char *line) = NULL;

/* -------------------------------------------------------------------------- */
/* 配置读写（NVS）                                                              */
/* -------------------------------------------------------------------------- */
int net_config_load(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NET_CFG_NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK)
    {
        ESP_LOGI(TAG, "NVS 无 netcfg 配置（尚未下发服务器地址）");
        return -1;
    }

    size_t len = sizeof(s_host);
    memset(s_host, 0, sizeof(s_host));
    err = nvs_get_str(h, NET_CFG_KEY_HOST, s_host, &len);
    if (err != ESP_OK || s_host[0] == '\0')
    {
        nvs_close(h);
        ESP_LOGI(TAG, "NVS 中无服务器地址");
        return -1;
    }

    uint16_t port = 0;
    if (nvs_get_u16(h, NET_CFG_KEY_PORT, &port) != ESP_OK || port == 0)
    {
        nvs_close(h);
        ESP_LOGW(TAG, "NVS 中端口无效");
        return -1;
    }

    uint8_t en = 0;
    nvs_get_u8(h, NET_CFG_KEY_ENABLE, &en);
    nvs_close(h);

    s_port   = port;
    s_enable = (en != 0);

    ESP_LOGI(TAG, "已载入服务器配置: %s:%u (enable=%d)", s_host, s_port, s_enable);
    return 0;
}

int net_config_save(const char *host, uint16_t port)
{
    if (host == NULL || host[0] == '\0' || port == 0)
    {
        ESP_LOGE(TAG, "配置无效: host=%s port=%u", host ? host : "(null)", port);
        return -1;
    }
    if (strlen(host) >= NET_HOST_MAX_LEN)
    {
        ESP_LOGE(TAG, "host 过长(最大 %d)", NET_HOST_MAX_LEN - 1);
        return -1;
    }

    nvs_handle_t h;
    esp_err_t err = nvs_open(NET_CFG_NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "nvs_open 失败: %d", err);
        return -1;
    }

    err = nvs_set_str(h, NET_CFG_KEY_HOST, host);
    if (err == ESP_OK) err = nvs_set_u16(h, NET_CFG_KEY_PORT, port);
    if (err == ESP_OK) err = nvs_set_u8(h, NET_CFG_KEY_ENABLE, 1);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "写入 NVS 失败: %d", err);
        return -1;
    }

    /* 立即生效：更新内存并触发重连 */
    strncpy(s_host, host, sizeof(s_host) - 1);
    s_host[sizeof(s_host) - 1] = '\0';
    s_port                     = port;
    s_enable                   = true;

    ESP_LOGI(TAG, "服务器配置已保存并生效: %s:%u", s_host, s_port);

    /* 若当前有连接，主动断开以便用新地址重连 */
    if (s_sock >= 0)
    {
        shutdown(s_sock, SHUT_RDWR);
        close(s_sock);
        s_sock = -1;
    }
    return 0;
}

int net_config_get(char *host, uint16_t *port)
{
    if (s_host[0] == '\0' || s_port == 0) return -1;
    if (host) strncpy(host, s_host, NET_HOST_MAX_LEN - 1);
    if (port) *port = s_port;
    return 0;
}

bool net_is_enabled(void)   { return s_enable; }
bool net_is_connected(void) { return s_connected; }

void net_set_rx_callback(void (*cb)(const char *line)) { s_rx_cb = cb; }

/* -------------------------------------------------------------------------- */
/* 发送                                                                         */
/* -------------------------------------------------------------------------- */
int net_send_json(const char *json)
{
    if (json == NULL || s_tx_queue == NULL) return -1;
    if (!s_connected) return -1;

    size_t len = strlen(json);
    if (len == 0 || len > NET_LINE_MAX_LEN) return -1;

    char *copy = (char *)malloc(len + 1);
    if (copy == NULL) return -1;
    memcpy(copy, json, len + 1);

    if (xQueueSend(s_tx_queue, &copy, 0) != pdTRUE)
    {
        free(copy);
        return -1;
    }
    return 0;
}

int net_send_jsonf(const char *fmt, ...)
{
    char buf[NET_LINE_MAX_LEN];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n <= 0 || n >= (int)sizeof(buf)) return -1;
    return net_send_json(buf);
}

/* -------------------------------------------------------------------------- */
/* TCP 连接                                                                     */
/* -------------------------------------------------------------------------- */
static int tcp_connect(const char *host, uint16_t port)
{
    struct addrinfo hints = {0};
    hints.ai_family       = AF_INET;
    hints.ai_socktype     = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    struct addrinfo *res = NULL;
    int gai = getaddrinfo(host, port_str, &hints, &res);
    if (gai != 0 || res == NULL)
    {
        ESP_LOGW(TAG, "DNS 解析失败: %s (%d)", host, gai);
        return -1;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0)
    {
        ESP_LOGW(TAG, "socket 创建失败: errno=%d", errno);
        freeaddrinfo(res);
        return -1;
    }

    struct timeval tv = {.tv_sec = NET_TCP_CONN_TIMEOUT_S, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "正在连接 %s:%u ...", host, port);
    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0)
    {
        ESP_LOGW(TAG, "连接失败: errno=%d", errno);
        close(sock);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);

    /* 接收超时：不能让 recv 永久阻塞，否则发送队列无法及时处理 */
    tv.tv_sec  = NET_TCP_RX_TIMEOUT_S;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* 关闭 Nagle，减少小包延迟 */
    int nodelay = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

    ESP_LOGI(TAG, "=== 已连接服务器 %s:%u ===", host, port);
    return sock;
}

/* -------------------------------------------------------------------------- */
/* 主任务                                                                       */
/* -------------------------------------------------------------------------- */
static void net_task(void *arg)
{
    static char rx_buf[NET_LINE_MAX_LEN];
    static char line[NET_LINE_MAX_LEN];
    int         line_len = 0;

    ESP_LOGI(TAG, "net task started");

    for (;;)
    {
        /* ---------- 未启用 / 无配置：等待 ---------- */
        if (!s_enable || s_host[0] == '\0')
        {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }

        /* ---------- 建立连接 ---------- */
        if (s_sock < 0)
        {
            s_connected = false;
            s_sock      = tcp_connect(s_host, s_port);
            if (s_sock < 0)
            {
                vTaskDelay(pdMS_TO_TICKS(NET_TCP_RECONNECT_MS));
                continue;
            }
            s_connected = true;
            line_len    = 0;

            /* 丢弃旧连接积压的待发数据 */
            char *stale = NULL;
            while (xQueueReceive(s_tx_queue, &stale, 0) == pdTRUE)
            {
                if (stale) free(stale);
            }
        }

        /* ---------- 发送队列（非阻塞清空） ---------- */
        char *out = NULL;
        while (xQueueReceive(s_tx_queue, &out, 0) == pdTRUE)
        {
            if (out == NULL) continue;

            size_t len = strlen(out);
            char   frame[NET_LINE_MAX_LEN + 2];
            if (len > NET_LINE_MAX_LEN) len = NET_LINE_MAX_LEN;
            memcpy(frame, out, len);
            frame[len]     = '\n';
            frame[len + 1] = '\0';

            int sent = send(s_sock, frame, len + 1, 0);
            free(out);
            out = NULL;

            if (sent < 0)
            {
                ESP_LOGW(TAG, "发送失败 errno=%d，重连", errno);
                close(s_sock);
                s_sock      = -1;
                s_connected = false;
                break;
            }
        }

        if (s_sock < 0) continue; /* 上一步已断开 */

        /* ---------- 接收（有超时，用于回到循环处理发送队列） ---------- */
        int n = recv(s_sock, rx_buf, sizeof(rx_buf), 0);
        if (n > 0)
        {
            for (int i = 0; i < n; i++)
            {
                char ch = rx_buf[i];
                if (ch == '\n')
                {
                    line[line_len] = '\0';
                    if (line_len > 0)
                    {
                        if (line[line_len - 1] == '\r') line[line_len - 1] = '\0';
                        ESP_LOGI(TAG, "RX << %s", line);
                        if (s_rx_cb) s_rx_cb(line);
                    }
                    line_len = 0;
                }
                else if (line_len < (int)sizeof(line) - 1)
                {
                    line[line_len++] = ch;
                }
                else
                {
                    ESP_LOGW(TAG, "下行行超长，丢弃");
                    line_len = 0;
                }
            }
        }
        else if (n == 0)
        {
            ESP_LOGW(TAG, "服务器关闭了连接");
            close(s_sock);
            s_sock      = -1;
            s_connected = false;
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        else
        {
            /* 超时(errno=EAGAIN/EWOULDBLOCK)属正常，回到循环顶部继续处理发送队列 */
            if (errno != EAGAIN && errno != EWOULDBLOCK)
            {
                ESP_LOGW(TAG, "recv 错误 errno=%d，重连", errno);
                close(s_sock);
                s_sock      = -1;
                s_connected = false;
                vTaskDelay(pdMS_TO_TICKS(500));
            }
        }
    }
}

void net_client_start(void)
{
    if (s_tx_queue == NULL)
    {
        s_tx_queue = xQueueCreate(NET_SEND_QUEUE_LEN, sizeof(char *));
    }
    /* 栈给足: 涉及 DNS 解析与 socket 操作 */
    xTaskCreate(net_task, "net", 1024 * 6, NULL, 4, NULL);
}
