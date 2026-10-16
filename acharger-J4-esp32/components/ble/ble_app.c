/*!
 * @file ble_app.c
 * @brief BLE 应用层命令分发实现（主机模式）
 *
 * 取代原「BLE 收到 -> 转发给 GD32/CCU」的逻辑，由 ESP32 自行解析处理。
 */

#include "ble_app.h"
#include "ble_config.h"
#include "esp32_ble_server.h"
#include "tinyml.h"
#include "tiny_sine.h"
#include "net_client.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <string.h>
#include <math.h>

static const char *TAG = "BLE-App";

/* 启用 WiFi 处理：收到 0x59 后直接调用 esp32_wifi_* 接口连接.
 * 需要 ble 组件在 CMakeLists 中依赖 wifi 组件（已配置）. */
#ifndef BLE_APP_ENABLE_WIFI
#define BLE_APP_ENABLE_WIFI (1)
#endif

#if BLE_APP_ENABLE_WIFI
#include "esp32_wifi.h"
#include "esp_wifi.h"
#endif

static bool s_report_tinyml = true;

/* WiFi 连接状态跟踪（用于异步上报 0x5A 的第 2 段） */
static volatile bool s_net_pending    = false; /* 有未完成的连接请求 */
static volatile bool s_net_ok         = false; /* 上一次连接结果 */
static volatile bool s_config_received = false; /* 是否收到过 WiFi 配置 */

/* -------------------------------------------------------------------------- */
/* 发送应答（统一走 0xABF2 数据通知）                                            */
/* -------------------------------------------------------------------------- */
static void ble_app_send(const uint8_t *data, uint16_t len)
{
    /* 无 BLE 中央设备连接时直接跳过。
     * 否则 esp_ble_gatts_send_indicate 会返回错误并刷屏警告
     *（TinyML 周期推送每 2 秒一次，未连接时必然失败）。 */
    if (!esp32_ble_is_connected())
    {
        return;
    }

    int ret = esp32_gatt_send_data((void *)data, len);
    if (ret != 0)
    {
        ESP_LOGW(TAG, "notify failed (ret=%d, len=%u)，可能未订阅", ret, len);
    }
}

/* -------------------------------------------------------------------------- */
/* 0x59 设置网络通信方式                                                        */
/* -------------------------------------------------------------------------- */
static void ble_app_net_set(const uint8_t *p, uint16_t len)
{
    if (len < 4)
    {
        ESP_LOGE(TAG, "net_set 长度不足: %u", len);
        uint8_t ack[4] = {BLE_CMD_NET_ACK, BLE_ACK_FAIL, 0x00, 0x00};
        ble_app_send(ack, sizeof(ack));
        return;
    }

    uint8_t net_type = p[0]; /* 0x00=以太网 0x01=4G 0x02=WiFi */
    uint8_t ip_mode  = p[1]; /* 0x00=静态 0x01=动态 */
    uint8_t ssid_len = p[2];
    uint8_t pwd_len  = p[3];

    ESP_LOGI(TAG, "net_set: net_type=%u ip_mode=%u ssid_len=%u pwd_len=%u",
             net_type, ip_mode, ssid_len, pwd_len);

    if (net_type != 0x02)
    {
        ESP_LOGW(TAG, "当前仅支持 WiFi(0x02)，收到 net_type=%u", net_type);
        uint8_t ack[4] = {BLE_CMD_NET_ACK, BLE_ACK_FAIL, 0x00, 0x00};
        ble_app_send(ack, sizeof(ack));
        return;
    }

    if ((uint16_t)(4 + ssid_len + pwd_len) > len)
    {
        ESP_LOGE(TAG, "net_set 载荷长度不符: 需要 %u, 实际 %u", 4 + ssid_len + pwd_len, len);
        uint8_t ack[4] = {BLE_CMD_NET_ACK, BLE_ACK_FAIL, 0x00, 0x00};
        ble_app_send(ack, sizeof(ack));
        return;
    }

    /* 立即回「收到配置=成功, 网络=进行中, MQTT=未开始」 */
    uint8_t ack[4] = {BLE_CMD_NET_ACK, BLE_ACK_OK, 0x00, 0x00};
    ble_app_send(ack, sizeof(ack));

#if BLE_APP_ENABLE_WIFI
    /* 拷贝出 SSID / 密码（加终止符，便于日志打印） */
    static char ssid[33];
    static char pwd[65];
    uint8_t sl = ssid_len > 32 ? 32 : ssid_len;
    uint8_t pl = pwd_len > 64 ? 64 : pwd_len;

    memset(ssid, 0, sizeof(ssid));
    memset(pwd, 0, sizeof(pwd));
    memcpy(ssid, &p[4], sl);
    memcpy(pwd, &p[4 + ssid_len], pl);

    ESP_LOGI(TAG, "WiFi 配置: SSID=\"%s\"(%u) 密码长度=%u", ssid, sl, pl);

    int r1 = esp32_wifi_set_ssid(ssid, sl);
    int r2 = esp32_wifi_set_passwd(pwd, pl);
    int r3 = esp32_wifi_connect();

    if (r1 != 0 || r2 != 0 || r3 != 0)
    {
        ESP_LOGE(TAG, "WiFi 配置失败: set_ssid=%d set_pwd=%d connect=%d "
                      "(通常表示 esp32_wifi_init 未调用)", r1, r2, r3);
        uint8_t nack[4] = {BLE_CMD_NET_ACK, BLE_ACK_OK, BLE_ACK_FAIL, 0x00};
        ble_app_send(nack, sizeof(nack));
        return;
    }

    /* 更新上报状态：网络连接中 */
    s_config_received = true;
    s_net_pending     = true;
    s_net_ok          = false;
    ESP_LOGI(TAG, "WiFi 已下发，等待连接结果…");
#else
    ESP_LOGW(TAG, "WiFi 处理未启用（BLE_APP_ENABLE_WIFI=0），配置已忽略");
#endif
}

/* -------------------------------------------------------------------------- */
/* 0x61 查询硬件信息                                                            */
/* -------------------------------------------------------------------------- */
static void ble_app_hw_info(void)
{
    uint8_t buf[42];
    memset(buf, 0, sizeof(buf));

    uint16_t i = 0;
    buf[i++] = BLE_CMD_HW_ACK;
    buf[i++] = BLE_PROTOCOL_TYPE;                     /* 协议类型 0x11 */
    buf[i++] = BLE_PROTOCOL_VER_MAJOR;                /* 协议版本 主 */
    buf[i++] = BLE_PROTOCOL_VER_MINOR;                /* 协议版本 次 */

    {
        size_t n = strlen(BLE_HW_NAME);
        memcpy(&buf[i], BLE_HW_NAME, n > 16 ? 16 : n);
    }
    i += 16;

    buf[i++] = BLE_HW_VER_MAJOR;
    buf[i++] = BLE_HW_VER_MINOR;
    buf[i++] = BLE_HW_VER_PATCH;

    {
        size_t n = strlen(BLE_SW_NAME);
        memcpy(&buf[i], BLE_SW_NAME, n > 16 ? 16 : n);
    }
    i += 16;

    buf[i++] = BLE_SW_VER_MAJOR;
    buf[i++] = BLE_SW_VER_MINOR;
    buf[i++] = BLE_SW_VER_PATCH;

    ESP_LOGI(TAG, "hw_info: %u 字节", i);
    ble_app_send(buf, i);
}

/* -------------------------------------------------------------------------- */
/* 0xE0/0xE1/0xE2 TinyML 控制                                                   */
/* -------------------------------------------------------------------------- */
static void ble_app_tinyml_ctrl(uint8_t cmd, const uint8_t *p, uint16_t len)
{
    uint8_t resp[2];

    if (cmd == BLE_CMD_TINYML_STOP)
    {
        tinyml_stop();
        resp[0] = BLE_CMD_TINYML_STOP;
        resp[1] = BLE_ACK_OK;
        ble_app_send(resp, 2);
        ESP_LOGI(TAG, "TinyML 周期推理已停止");
    }
    else if (cmd == BLE_CMD_TINYML_START)
    {
        tinyml_start();
        resp[0] = BLE_CMD_TINYML_START;
        resp[1] = BLE_ACK_OK;
        ble_app_send(resp, 2);
        ESP_LOGI(TAG, "TinyML 周期推理已启动");
    }
    else if (cmd == BLE_CMD_TINYML_RUN)
    {
        if (len < 4)
        {
            ESP_LOGE(TAG, "TinyML 单次推理缺少 float 参数");
            resp[0] = BLE_CMD_TINYML_RUN;
            resp[1] = BLE_ACK_FAIL;
            ble_app_send(resp, 2);
            return;
        }

        float x;
        memcpy(&x, p, 4); /* 小端 float */
        float y = tiny_sine_predict(x - floorf(x));

        uint8_t out[5];
        out[0] = BLE_CMD_TINYML_RUN;
        memcpy(&out[1], &y, 4);
        ble_app_send(out, sizeof(out));

        ESP_LOGI(TAG, "TinyML 单次推理: x=%.4f -> y=%+.4f", x, y);
    }
}

/* -------------------------------------------------------------------------- */
/* 0xE4 下发云端服务器地址（写入 NVS 并立即生效）                                */
/*   载荷: [portLo][portHi][host...]  (host 以 '\0' 结尾)                        */
/*   应答: [0xE4][ok:1][portLo][portHi]                                          */
/* -------------------------------------------------------------------------- */
static void ble_app_net_srv_set(const uint8_t *p, uint16_t len)
{
    uint8_t resp[4] = {BLE_CMD_NET_SRV_ACK, BLE_ACK_FAIL, 0x00, 0x00};

    if (len < 3)
    {
        ESP_LOGE(TAG, "0xE4 载荷不足: %u", len);
        ble_app_send(resp, sizeof(resp));
        return;
    }

    /* 小端端口 */
    uint16_t port = (uint16_t)(p[0] | (p[1] << 8));

    /* 主机名: 剩余部分，要求以 '\0' 结尾（或整段长度即为字符串） */
    const char *host = (const char *)&p[2];
    uint16_t    host_len = len - 2;

    /* 防御: 确保有终止符 */
    static char host_buf[NET_HOST_MAX_LEN];
    uint16_t copy_len = host_len;
    if (copy_len >= sizeof(host_buf)) copy_len = sizeof(host_buf) - 1;
    memcpy(host_buf, host, copy_len);
    host_buf[copy_len] = '\0';

    ESP_LOGI(TAG, "0xE4 下发服务器: \"%s\":%u", host_buf, port);

    if (host_buf[0] == '\0' || port == 0)
    {
        ESP_LOGE(TAG, "服务器地址或端口无效");
        ble_app_send(resp, sizeof(resp));
        return;
    }

    if (net_config_save(host_buf, port) == 0)
    {
        resp[1] = BLE_ACK_OK;
        resp[2] = (uint8_t)(port & 0xFF);
        resp[3] = (uint8_t)((port >> 8) & 0xFF);
        ESP_LOGI(TAG, "服务器配置已保存到 NVS 并生效");
    }
    else
    {
        ESP_LOGE(TAG, "保存服务器配置失败");
    }

    ble_app_send(resp, sizeof(resp));
}

/* -------------------------------------------------------------------------- */
/* TinyML 结果回调：把周期推理结果推给 BLE 客户端                                 */
/* -------------------------------------------------------------------------- */
static void on_tinyml_result(float phase, float pred)
{
    if (!s_report_tinyml)
    {
        return;
    }

    /* 载荷: [0xE2][float phase][float pred] */
    uint8_t out[9];
    out[0] = BLE_CMD_TINYML_RUN;
    memcpy(&out[1], &phase, 4);
    memcpy(&out[5], &pred, 4);
    ble_app_send(out, sizeof(out));
}

/* -------------------------------------------------------------------------- */
/* 统一入口                                                                     */
/* -------------------------------------------------------------------------- */
void ble_app_handle_data(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0)
    {
        return;
    }

    uint8_t cmd = data[0];
    ESP_LOGI(TAG, "收到命令 0x%02X, 长度 %u", cmd, len);

    switch (cmd)
    {
    case BLE_CMD_NET_SET:
        ble_app_net_set(&data[1], len - 1);
        break;

    case BLE_CMD_HW_QUERY:
        ble_app_hw_info();
        break;

    case BLE_CMD_TINYML_STOP:
    case BLE_CMD_TINYML_START:
        ble_app_tinyml_ctrl(cmd, NULL, 0);
        break;

    case BLE_CMD_TINYML_RUN:
        ble_app_tinyml_ctrl(cmd, &data[1], len - 1);
        break;

    case BLE_CMD_NET_SRV_SET:
        ble_app_net_srv_set(&data[1], len - 1);
        break;

    default:
        ESP_LOGW(TAG, "未识别的命令 0x%02X（长度 %u）", cmd, len);
        break;
    }
}

void ble_app_set_tinyml_report(bool on)
{
    s_report_tinyml = on;
    ESP_LOGI(TAG, "TinyML 结果上报: %s", on ? "开" : "关");
}

/* -------------------------------------------------------------------------- */
/* WiFi 初始化 + 状态轮询任务                                                    */
/* -------------------------------------------------------------------------- */
/* 说明: esp32_wifi_init() 内部会调用 esp_wifi_init / esp_netif_create_default_wifi_sta
 *       等重栈操作。若直接放在 app_main()（main 任务栈默认约 3.5KB）里执行，
 *       叠加 BLE 栈后极易栈溢出，表现为上电后随机重启（RTC_SW_SYS_RST）。
 *       因此这里用独立任务、给足栈空间来执行初始化。 */
#if BLE_APP_ENABLE_WIFI
#define WIFI_TASK_STACK (1024 * 5)

static void wifi_state_task(void *arg)
{
    ESP_LOGI(TAG, "WiFi 初始化任务启动（栈 %d 字节）", WIFI_TASK_STACK);

    /* 主机模式: 初始化后不自动连接，等 BLE 下发配置。
     * 避免用空 SSID 先发起一次无效连接（会占用重试次数并与 BLE 抢射频）。 */
    esp32_wifi_set_auto_connect(false);

    /* ---- 第一步: 初始化 WiFi（耗时且吃栈，放在本任务里做）---- */
    if (esp32_wifi_init() == 0)
    {
        ESP_LOGI(TAG, "WiFi 初始化完成");
    }
    else
    {
        ESP_LOGW(TAG, "WiFi 初始化返回非 0（可能已初始化）");
    }

    /* ---- 第二步: 设置 WiFi 发射功率 ----
     * BLE + WiFi 共存时射频同时工作，峰值电流较大；但功率过低会导致弱信号下
     * beacon 丢失、连上后迅速掉线。这里取 15dBm 作为折中（默认最大约 20dBm）。 */
    {
        int8_t pwr = 60; /* 单位 0.25dBm → 60*0.25 = 15dBm */
        esp_err_t e = esp_wifi_set_max_tx_power(pwr);
        if (e == ESP_OK)
        {
            int8_t read_back = 0;
            esp_wifi_get_max_tx_power(&read_back);
            ESP_LOGI(TAG, "WiFi 发射功率已设为 %d (%.2f dBm)", read_back, read_back * 0.25);
        }
        else
        {
            ESP_LOGW(TAG, "设置 WiFi 发射功率失败: %d", e);
        }
    }

    /* ---- 第三步: 周期轮询连接状态 ----
     * get_wifi_state(): 0 = 已连接, 其他 = 未连接
     *
     * 状态机:
     *   - 初次进入且状态为"未连接"  -> 仅记录基线，不打扰（避免开机就报离线）
     *   - 状态变化                  -> 打印日志，并在"曾下发过配置"的前提下上报 0x5A
     *   - 连接成功                  -> 上报 网络=成功(0x01)
     *   - 掉线                      -> 上报 网络=失败(0x00)，让 APP 立刻看到离线
     */
    int last_st = -1;      /* -1 = 尚未建立基线 */
    bool baseline_set = false;

    for (;;)
    {
        bool up = (get_wifi_state() == 0);

        if (!baseline_set)
        {
            last_st      = up;
            baseline_set = true;
            ESP_LOGI(TAG, "WiFi 初始状态: %s", up ? "已连接" : "未连接");
        }
        else if (up != last_st)
        {
            ESP_LOGI(TAG, "=== WiFi 状态变化: %s ===", up ? "已连接(在线)" : "未连接(离线)");
            last_st = up;

            /* 仅在下发过配置后上报，避免开机未配置时反复打扰 APP */
            if (s_config_received)
            {
                uint8_t ack[4] = {
                    BLE_CMD_NET_ACK,
                    BLE_ACK_OK,
                    up ? BLE_ACK_OK : BLE_ACK_FAIL,
                    0x00};
                ble_app_send(ack, sizeof(ack));
                ESP_LOGI(TAG, "已上报 0x5A: 网络=%s", up ? "成功" : "失败");
            }

            if (up)
            {
                s_net_pending = false;
                s_net_ok      = true;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
#endif

void ble_app_init(void)
{
    /* 注册推理结果回调，周期任务每次推理后自动推送给 BLE 客户端 */
    tinyml_set_result_callback(on_tinyml_result);

#if BLE_APP_ENABLE_WIFI
    /* WiFi 初始化 + 状态轮询统一放到独立任务，避免在 app_main 里栈溢出 */
    xTaskCreate(wifi_state_task, "wifi_state", WIFI_TASK_STACK, NULL, 3, NULL);
#endif

    ESP_LOGI(TAG, "BLE 应用层已初始化（命令分发 + TinyML 结果上报）");
}
