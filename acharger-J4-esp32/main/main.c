#include "esp32_uart.h"
#include "esp32_wifi.h"
#include "esp32_ble_server.h"
#include "ble_app.h"
#include "tinyml.h"
#include "net_client.h"
#include "net_report.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* 内存监控开关: 1 = 每 SHOW_DELAY 毫秒打印一次堆使用情况
 * WiFi + BLE 共存时用于观察内存开销 */
#define SHOW_STATE 1
#define SHOW_MEM   1
#define SHOW_DELAY (10000) /* 10 秒一次 */

/* 主机模式开关：1 = 用代码内写死的参数启动 BLE，不等 CCU 下发 */
#define BLE_START_LOCALLY 1

void app_main(void)
{
    esp32_uart_start();

#if BLE_START_LOCALLY
    /* BLE 主机模式：参数见 components/ble/ble_config.h */
    esp32_ble_start_with_config();
#endif

    /* BLE 应用层：注册命令分发与 TinyML 结果回调 */
    ble_app_init();

    /* 云端上报: 载入 NVS 中的服务器配置（BLE 下发过才会有），
     * 并启动 TCP 客户端与上报任务。未配置时仅等待，不做连接。 */
    net_config_load();
    net_report_init();
    net_client_start();

    /* TinyML: 启动正弦波推理任务（默认 2s 一次，可由 BLE 远程启停） */
    tinyml_task_start();

#if SHOW_STATE
    {
        while (1)
        {
            vTaskDelay(SHOW_DELAY / portTICK_PERIOD_MS);
#if SHOW_MEM
            size_t free_now  = xPortGetFreeHeapSize();
            size_t free_min  = esp_get_minimum_free_heap_size();
            size_t largest   = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

            ESP_LOGI("MEM", "free=%u B (%.1f KB)  min_ever=%u B (%.1f KB)  max_block=%u B",
                     (unsigned)free_now, free_now / 1024.0,
                     (unsigned)free_min, free_min / 1024.0,
                     (unsigned)largest);
#endif
        }
    }
#endif
}
