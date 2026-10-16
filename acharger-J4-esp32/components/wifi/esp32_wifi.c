#include "esp32_wifi.h"
#include "esp32_uart.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "nvs_flash.h"
#include <string.h>

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
#define WIFI_TAG           "WIFI"
#define RETRY              (5)

static EventGroupHandle_t s_wifi_event_group;
esp_netif_t *sta_netif = NULL;

static_ip_set ip_set;
static int retry_num = 0;
wifi_config_t wifi_config;
static uint8_t sta_ssid[32];
uint8_t sta_ssid_len;

uint8_t esp32_wifi_conn    = 1;

static bool wifi_init_done = false;

/* 是否在 esp_wifi_start() 后立即自动连接。
 * 主机模式(由 BLE 下发配置)下应为 false，等配置到位再连。 */
bool auto_connect = false;

/* 是否期望保持连接：掉线后据此决定是否重连。
 * esp32_wifi_connect() 置 true，esp32_wifi_disconnect() 置 false。 */
static volatile bool expect_connect = false;

void esp32_wifi_set_auto_connect(bool enable)
{
    auto_connect = enable;
    ESP_LOGI(WIFI_TAG, "auto_connect = %d", enable);
}

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    wifi_event_sta_connected_t *event;
    if (event_base == WIFI_EVENT)
    {
        switch (event_id)
        {
        case WIFI_EVENT_STA_START:
        {
            /* 主机模式下由 BLE 下发配置后才连接。
             * 若此处无条件 esp_wifi_connect()，会先用空 SSID 发起一次连接，
             * 既浪费重试次数，也会在 BLE 共存期间引入额外的射频时序竞争。 */
            if (!auto_connect)
            {
                ESP_LOGI(WIFI_TAG, "WiFi started, 等待 BLE 下发配置后再连接");
                break;
            }
            esp_wifi_connect();
            break;
        }
        case WIFI_EVENT_STA_CONNECTED:
        {
            event = (wifi_event_sta_connected_t *)event_data;
            memcpy(sta_ssid, event->ssid, event->ssid_len);
            sta_ssid_len = event->ssid_len;
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED:
        {
            /* 无条件重连（只要仍期望连接）。
             *
             * 原实现在此处判断 `esp32_wifi_conn == 1`，但该变量在 GOT_IP 时已被
             * 置 0（表示"已连接"）。于是"连上后掉线"的场景下判断恒为假，
             * 会直接放弃重连 —— 表现为连上几秒后彻底断开、再也不恢复。 */
            esp32_wifi_conn = 1;
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);

            if (expect_connect)
            {
                /* 注意: 事件回调中不可长时间阻塞(会拖垮事件任务)，
                 * 故不使用 vTaskDelay，而是持续重连；ESP-IDF 自身在
                 * WIFI_EVENT_STA_DISCONNECTED 后会有内部退避。 */
                if (retry_num >= RETRY)
                {
                    ESP_LOGW(WIFI_TAG, "已重试 %d 次, 继续重连", retry_num);
                    retry_num = 0;
                }
                esp_wifi_connect();
                retry_num++;
                ESP_LOGI(WIFI_TAG, "retry to connect to the AP (retry=%d)", retry_num);
            }
            else
            {
                ESP_LOGI(WIFI_TAG, "disconnect to the AP (未期望连接, 不重连)");
            }
            break;
        }
        }
    }
    else if (event_base == IP_EVENT)
    {
        if (event_id == IP_EVENT_STA_GOT_IP)
        {
            esp32_wifi_conn          = 0;
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            ESP_LOGI(WIFI_TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
            retry_num = 0;

            esp_netif_dns_info_t dns;
            esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns);
            ESP_LOGI(WIFI_TAG, "DNS : " IPSTR ", DNS type = %u", IP2STR(&dns.ip.u_addr.ip4), dns.ip.type);

            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
            /* 主机模式: 不再向 CCU 上报（原实现在 WiFi 事件回调里 malloc+入队，
             * 会加重事件任务栈负担）。APP 侧的 0x5A 应答由 ble_app 轮询上报。 */
        }
    }
}

int esp32_wifi_set_ssid(char *ssid, uint8_t len_ssid)
{
    if (!wifi_init_done)
    {
#if INF_LOG_ENABLE
        ESP_LOGI(WIFI_TAG, "wifi not init yet!");
#endif
        return -1;
    }

    // set SSID
    memcpy((char *)wifi_config.sta.ssid, ssid, len_ssid);
    wifi_config.sta.ssid[len_ssid] = '\0';

    // set Wifi config
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
#if INF_LOG_ENABLE
    ESP_LOGI(WIFI_TAG, "Recv STA SSID:   %s\n", wifi_config.sta.ssid);
#endif
    return 0;
}

int esp32_wifi_set_passwd(char *passwd, uint8_t len_passwd)
{
    if (!wifi_init_done)
    {
#if INF_LOG_ENABLE
        ESP_LOGI(WIFI_TAG, "wifi not init yet!");
#endif
        return -1;
    }
    // set PASSWD
    memcpy((char *)wifi_config.sta.password, passwd, len_passwd);
    wifi_config.sta.password[len_passwd] = '\0';

    // set Wifi config
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
#if INF_LOG_ENABLE
    ESP_LOGI(WIFI_TAG, "Recv STA PASSWD: %s\n", wifi_config.sta.password);
#endif
    return 0;
}

int esp32_wifi_disconnect(void)
{
    if (!wifi_init_done)
    {
#if INF_LOG_ENABLE
        ESP_LOGI(WIFI_TAG, "wifi not init yet!");
#endif
        return -1;
    }
    retry_num = RETRY;
    expect_connect = false; /* 主动断开后不再自动重连 */
    esp_wifi_disconnect();
    return 0;
}

int esp32_wifi_connect(void)
{
    if (!wifi_init_done)
    {
#if INF_LOG_ENABLE
        ESP_LOGI(WIFI_TAG, "wifi not init yet!");
#endif
        return -1;
    }

    expect_connect = true; /* 期望保持连接，掉线后自动重连 */
    if (esp32_wifi_conn == 0)
    {
        esp_wifi_disconnect();
    }
    retry_num = 0;
    esp_wifi_connect();
    return 0;
}

int esp32_wifi_dhcp_start()
{
    if (sta_netif)
    {
        return esp_netif_dhcpc_start(sta_netif);
    }
    else
    {
#if INF_LOG_ENABLE
        ESP_LOGI(WIFI_TAG, "wifi not init yet!");
#endif
        return -1;
    }

    return 0;
}

int esp32_wifi_static_ip(static_ip_set *ip_set)
{
    if (sta_netif)
    {
        esp_err_t err;
        err = esp_netif_dhcpc_stop(sta_netif);
        if (err != 0)
        {
            return -1;
        }

        esp_netif_ip_info_t ip_info = {0};
        ip_info.ip.addr             = ESP_IP4TOADDR(ip_set->ip[0], ip_set->ip[1], ip_set->ip[2], ip_set->ip[3]);
        ip_info.gw.addr             = ESP_IP4TOADDR(ip_set->gw[0], ip_set->gw[1], ip_set->gw[2], ip_set->gw[3]);
        ip_info.netmask.addr =
            ESP_IP4TOADDR(ip_set->netmask[0], ip_set->netmask[1], ip_set->netmask[2], ip_set->netmask[3]);
        err = esp_netif_set_ip_info(sta_netif, &ip_info);
        if (err != 0)
        {
            return -1;
        }

        esp_netif_dns_info_t dns_info = {0};
        dns_info.ip.u_addr.ip4.addr   = ESP_IP4TOADDR(ip_set->dns[0], ip_set->dns[1], ip_set->dns[2], ip_set->dns[3]);
        dns_info.ip.type              = 0;
        err                           = esp_netif_set_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns_info);
        if (err != 0)
        {
            return -1;
        }

        return err;
    }
    else
    {
#if INF_LOG_ENABLE
        ESP_LOGI(WIFI_TAG, "wifi not init yet!");
#endif
        return -1;
    }

    return 0;
}

void wifi_init_nvs(void)
{ // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
#if INF_LOG_ENABLE
    ESP_LOGI(WIFI_TAG, "wifi_init_nvs finished.");
#endif
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    sta_netif              = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(
        esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &instance_got_ip));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

#if INF_LOG_ENABLE
    ESP_LOGI(WIFI_TAG, "wifi_init_sta finished.");
#endif
}

uint8_t get_wifi_state() { return esp32_wifi_conn; }

int esp32_wifi_init(void)
{
    if (wifi_init_done)
    {
#if INF_LOG_ENABLE
        ESP_LOGI(WIFI_TAG, "wifi init already!");
#endif
        return -1;
    }

    wifi_init_nvs();
    wifi_init_sta();
    memset(sta_ssid, 0, sizeof(sta_ssid));
    wifi_init_done = true;

    return 0;
}
