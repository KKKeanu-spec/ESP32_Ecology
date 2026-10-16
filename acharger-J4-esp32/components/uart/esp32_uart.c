#include "esp32_uart.h"
#include "driver/uart.h"
#include "esp32_ble_server.h"
#include "esp32_sockets.h"
#include "esp32_tls.h"
#include "esp32_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

#define CHECK_ENABLED  0
#define NEED_CONFIRM   0

#define UART_HEAD      (0x55aa) // 报文头
#define CHECK          (0xff)   // 校验位
#define SIZE_OF_CHECK  (1U)     // 校验长度

#define ESP32_UART_POT UART_NUM_1
#define ESP32_UART_TXD (7)
#define ESP32_UART_RXD (6)
#define ESP32_UART_RTS (-1)
#define ESP32_UART_CTS (-1)

#define BUF_FULLSIZE   (1024 * 4)

#define BUF_SIZE       (2)
#define BUF_FLAG       (4)
#define BUF_DATA       (5)
#define ANS_SIZE       (7)

static QueueHandle_t uart_queue;
static char buf_read[BUF_FULLSIZE];
static char rx_buffer[BUF_FULLSIZE];

#if NEED_CONFIRM
static QueueHandle_t recv_handle_queue = NULL;
static uint8_t recv_confirm            = -1;
static uint8_t confirm_timeout         = 20; // seconds
#endif

static QueueHandle_t send_buffer_queue = NULL;

static QueueHandle_t server_set_queue  = NULL;
static TaskHandle_t server_recv_handle = NULL;
static uint8_t ble_init_done           = 0;
static int8_t net_mode                 = -1;

#define UART_TAG   "UART"
#define SERVER_TAG "Server"
#define RECV_TAG   "Recv"
#define SEND_TAG   "Send"
#define WIFI_TAG   "Wifi"
#define BLE_TAG    "BLE"

uint8_t uart_check_set(char *buffer, uint16_t size)
{
    uint8_t check = CHECK;
#if CHECK_ENABLED
    for (int i = 0; i < size - 1; i++)
    {
        check ^= buffer[i];
    }
#endif
    return check;
}

bool uart_recv_confirm(uint8_t cmd)
{
#if NEED_CONFIRM
    if (cmd == UART_SEND_BLE_STATE || cmd == UART_SEND_WIFI_STATE || cmd == UART_SEND_SERVER_STATE ||
        cmd == UART_SEND_DATA_RECV)
    {
        if (xQueueReceive(recv_handle_queue, &recv_confirm, confirm_timeout * 1000 / portTICK_PERIOD_MS))
        {
            if (recv_confirm == 0)
            {
#if INF_LOG_ENABLE
                ESP_LOGI(RECV_TAG, "Data revc confimed");
#endif
                recv_confirm = -1;
                return false;
            }
        }
#if INF_LOG_ENABLE
        ESP_LOGI(RECV_TAG, "No confirmation received after %d seconds, send data to GD32 again", confirm_timeout);
#endif
        ESP_LOGE(RECV_TAG, "Resend cmd: %X", cmd);
        recv_confirm = -1;
        return true;
    }
#endif
    return false;
}

void uart_send_task(void *args)
{
    send_buffer buffer;
    ESP_LOGI(UART_TAG, "uart_send_task started");
    for (;;)
    {
        xQueueReceive(send_buffer_queue, &buffer, portMAX_DELAY);
        do
        {
#if INF_LOG_ENABLE
#if DATA_LOG_ENABLE
            ESP_LOGI(UART_TAG, "Ready to send data to GD32, len = %d", buffer.len);
#endif
#if HEX_LOG_ENABLE
            ESP_LOG_BUFFER_HEXDUMP(UART_TAG, buffer.addr, buffer.len, ESP_LOG_WARN);
#endif
#endif
            uart_write_bytes(ESP32_UART_POT, (void *)buffer.addr, buffer.len);
#if INF_LOG_ENABLE
#if DATA_LOG_ENABLE
            ESP_LOGI(UART_TAG, "Send data to GD32 complete!");
#endif
#endif
        } while (uart_recv_confirm(((uint8_t *)(buffer.addr))[4]));
        free(buffer.addr);
        vTaskDelay(20 / portTICK_PERIOD_MS);
    }
}

int esp32_uart_send_data(uint8_t cmd, uint8_t flag, char *data, int len)
{
    uint16_t len_all     = sizeof(uart_send) + len + SIZE_OF_CHECK;

    uart_send *send_data = (uart_send *)malloc(len_all);
    bzero(send_data, len_all);

    send_data->head = UART_HEAD;
    send_data->size = len_all - sizeof(send_data->head) - sizeof(send_data->size);
    send_data->cmd  = cmd;
    send_data->flag = flag;
    if (len > 0)
    {
        memcpy(send_data->data, data, len);
    }
    send_data->data[len] = uart_check_set((char *)send_data, len_all);

    send_buffer buffer   = {
          .addr = send_data,
          .len  = len_all,
    };

    // ESP_LOGW(SEND_TAG, "Send cmd: %X", send_data->cmd);
    xQueueSend(send_buffer_queue, &buffer, 1000 / portTICK_PERIOD_MS);

    return 0;
}

int (*server_recv)(char *, uint16_t);

void server_recv_task(void *args)
{
    static int recv_len = 0;
    if (net_mode == NET_SOCKET)
    {
        server_recv = (int (*)(char *, uint16_t))esp32_socket_recv_data;
    }
    else if (net_mode == NET_TLS)
    {
        server_recv = (int (*)(char *, uint16_t))esp32_tls_recv_data;
    }
    else
    {
#if INF_LOG_ENABLE
        ESP_LOGE(RECV_TAG, "no mod set!");
#endif
        vTaskSuspend(server_recv_handle);
    }

    for (;;)
    {
        recv_len = 0;
        recv_len = server_recv(rx_buffer, (BUF_FULLSIZE));

        if (recv_len > 0)
        {
#if INF_LOG_ENABLE
#if DATA_LOG_ENABLE
            ESP_LOGI(RECV_TAG, "recieve data from server, length = %d", recv_len);
#endif
#if HEX_LOG_ENABLE
            ESP_LOG_BUFFER_HEXDUMP(RECV_TAG, rx_buffer, recv_len, ESP_LOG_WARN);
#endif
#endif
            esp32_uart_send_data(UART_SEND_DATA_RECV, NET_DATA, rx_buffer, recv_len);
        }
        else if (recv_len == 0)
        {
            // 空闲超时：连接仍然有效，只是当前没有数据（底层 EAGAIN 转换为 0）。
            // 注意：0 仅代表“空闲”，对端关闭(FIN)由 esp32_socket_recv_data 返回 -1，走下方 else 分支。
            // HTTP keep-alive 下 GD32 逐包发 Range GET、服务器逐段返回，两段响应之间
            // recv 必然空闲若干秒（SO_RCVTIMEO=5s 触发）。此时必须继续等待下一段响应，
            // 绝不能关闭 socket / 挂起任务，否则 OTA 会在“包间隙”被永久掐断。
            continue;
        }
        else
        {
#if INF_LOG_ENABLE
            ESP_LOGE(RECV_TAG, "Error occurred during recving");
#endif
            // recv 返回 <0：连接真正出错或对端关闭。
            //
            // net_mode == -1 表示 server_connect 正在【主动重建连接】：
            // 它为了释放旧 socket（避免 lwIP socket 泄漏）会先调用 esp32_socket_exit()，
            // 而本任务此刻正阻塞在 recv() 里，socket 被关会让 recv() 立刻返回
            // 错误（典型 errno 128 / ENOTCONN）。这是预期内的，并非真的断连。
            // 若不加以区分就上报 SERVER_STATE=NO，GD32 会收到【假断连】，
            // 把刚要恢复的 socket.connected 又清掉，导致每次断连都要重连两遍、
            // 白白多花一个 intervals 周期（日志表现为连续两条 [OTA] connectServer
            // 之后才终于 ota send）。因此主动重连期间必须抑制这次上报。
            if (net_mode == -1)
            {
#if INF_LOG_ENABLE
                ESP_LOGW(RECV_TAG, "recv aborted by local reconnect, skip reporting server down");
#endif
            }
            else
            {
                if (net_mode == NET_SOCKET)
                {
                    esp32_socket_exit();
                }
                else if (net_mode == NET_TLS)
                {
                    esp32_tls_exit();
                }
                esp32_uart_send_data(UART_SEND_SERVER_STATE, STATE_NO, NULL, 0);
            }
            vTaskSuspend(server_recv_handle);
        }
    }
}

int uart_send_data_to_wireless(char *buffer, char *size)
{
    uint8_t wireless_mode = (uint8_t)buffer[0];
    uint16_t data_size    = *(uint16_t *)size - 3;

    if (wireless_mode == NET_DATA)
    {
        static int len = 0;
#if INF_LOG_ENABLE
#if DATA_LOG_ENABLE
        ESP_LOGI(SEND_TAG, "Ready to send data to WIFI, length = %d", data_size);
#endif
#if HEX_LOG_ENABLE
        ESP_LOG_BUFFER_HEXDUMP(SEND_TAG, &buffer[1], data_size, ESP_LOG_WARN);
#endif
#endif
        if (net_mode == NET_SOCKET)
        {
            len = esp32_socket_send_data(&buffer[1], data_size);
        }
        else if (net_mode == NET_TLS)
        {
            len = esp32_tls_send_data(&buffer[1], data_size);
        }
        else
        {
#if INF_LOG_ENABLE
            ESP_LOGE(WIFI_TAG, "No net mod set yet!");
#endif
            return -1;
        }
#if INF_LOG_ENABLE
#if DATA_LOG_ENABLE
        ESP_LOGI(SEND_TAG, "Send data to WIFI complete!");
#endif
#endif
        uint8_t err = len > 0 ? STATE_OK : STATE_NO;
        esp32_uart_send_data(UART_ANSR_DATA_SEND, NET_DATA, (char *)&err, 1);
        return err;
    }
    else if (wireless_mode == BLE_DATA)
    {
        int err;
        if (ble_init_done)
        {
#if INF_LOG_ENABLE
            ESP_LOGI(SEND_TAG, "Ready to send data to BLE, length = %d", data_size);
#if HEX_LOG_ENABLE
            ESP_LOG_BUFFER_HEXDUMP(SEND_TAG, &buffer[1], data_size, ESP_LOG_WARN);
#endif
#endif
            err = esp32_gatt_send_data(&buffer[1], data_size);
#if INF_LOG_ENABLE
            ESP_LOGI(SEND_TAG, "Send data to BLE complete!");
#endif
        }
        else
        {
            err = -1;
#if INF_LOG_ENABLE
            ESP_LOGE(WIFI_TAG, "BLE not init yet!");
#endif
        }
        esp32_uart_send_data(UART_ANSR_DATA_SEND, BLE_DATA, (char *)&err, 1);
        return err;
    }
    else
    {
#if INF_LOG_ENABLE
        ESP_LOGE(UART_TAG, "Wireless mode type error!");
#endif
        return -1;
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);

    return 0;
}

int uart_server_set(char *buffer, char *len)
{
    uint16_t size     = *(uint16_t *)len - 2;
    uint8_t tls       = buffer[0];
    uint16_t port     = *(uint16_t *)&buffer[1];
    char *server_addr = (char *)malloc(size - 2);
    memcpy(server_addr, &buffer[3], size - 3);
    server_addr[size - 3] = '\0';

    server_info server    = {
           .tls  = tls,
           .port = port,
           .addr = server_addr,
    };

    xQueueSend(server_set_queue, &server, portMAX_DELAY);
    esp32_uart_send_data(UART_ANSR_SERVER_SET, STATE_OK, NULL, 0);

    return 0;
}

void server_connect(void *args)
{
    static int err   = -1;
    static int retry = 0;
    server_info server;
    ESP_LOGI(SERVER_TAG, "server_connect task started");
    for (;;)
    {
        xQueueReceive(server_set_queue, &server, portMAX_DELAY);

#if INF_LOG_ENABLE
        ESP_LOGI(WIFI_TAG, "mode set = %d", server.tls);
        ESP_LOGI(WIFI_TAG, "port = %d", server.port);
        ESP_LOGI(WIFI_TAG, "server = %s", server.addr);
#endif
        // 【关键修复】必须先关闭连接，再删除接收任务（顺序不能反）。
        // server_recv_task 是死循环，绝大多数时间正阻塞在 recv() 里（SO_RCVTIMEO=5s）。
        // 若先 vTaskDelete() 一个阻塞在 socket 上的任务，lwIP 来不及回收该 socket，
        // 会永久泄漏一个 fd；CONFIG_LWIP_MAX_SOCKETS=10，重连十来次后 socket()
        // 就分配失败（"Socket create failed"），此后所有重连必定失败，
        // 只能靠硬件复位 ESP32 才能恢复——表现为 OTA 中途反复掉连直至卡死。
        // 先关 socket 可让阻塞中的 recv() 立即返回，任务解除阻塞后再删除才安全。
        if (net_mode == NET_SOCKET)
        {
            esp32_socket_exit();
            net_mode = -1;
        }
        else if (net_mode == NET_TLS)
        {
            esp32_tls_exit();
            net_mode = -1;
        }

        // End the current receive thread if exist
        if (server_recv_handle != NULL)
        {
            // 给刚被解除阻塞的 recv 任务一点时间真正退出，
            // 避免在 lwIP 内部仍持有引用时就被删除。
            vTaskDelay(50 / portTICK_PERIOD_MS);
            vTaskDelete(server_recv_handle);
#if INF_LOG_ENABLE
            ESP_LOGI(SERVER_TAG, "recv task deleted");
#endif
            server_recv_handle = NULL;
        }

        retry = 0;

        // Connect server
        do
        {
            // Open new connection
            if (server.tls == NET_SOCKET)
            {
                net_mode = NET_SOCKET;
                esp32_socket_init();
                err = esp32_socket_connect(server.addr, server.port);
            }
            else if (server.tls == NET_TLS)
            {
                net_mode = NET_TLS;
                esp32_tls_init();
                err = esp32_tls_connect(server.addr, server.port);
            }
            else
            {
#if INF_LOG_ENABLE
                ESP_LOGE(WIFI_TAG, "server setting error!");
#endif
                break;
            }

            if (err == 0)
            {
                if (server_recv_handle == NULL)
                {
                    xTaskCreate(server_recv_task, "recv", 1024 * 8, NULL, 1, &server_recv_handle);
                }
#if INF_LOG_ENABLE
                ESP_LOGI(SERVER_TAG, "recv task create");
#endif
                retry = 0;
                break;
            }
            else
            {
                if (retry < 5)
                {
                    retry++;
                }
                else
                {
                    break;
                }
                vTaskDelay(5 * 1000 / portTICK_PERIOD_MS);
            }
        } while (true);
        esp32_uart_send_data(UART_SEND_SERVER_STATE, (uint8_t)err, NULL, 0);
        free(server.addr);
    }
}

int uart_wifi_set(char *buffer)
{
    esp32_uart_send_data(UART_ANSR_WIFI_SET,
                         (uint8_t)(esp32_wifi_set_ssid(&buffer[1], buffer[0]) ||
                                   esp32_wifi_set_passwd(&buffer[buffer[0] + 2], buffer[buffer[0] + 1])),
                         NULL, 0);
    return 0;
}

int uart_wifi_init()
{
    esp32_uart_send_data(UART_ANSR_WIFI_INIT, esp32_wifi_init(), NULL, 0);
    return 0;
}

int uart_wifi_dhcp_start()
{
    esp32_uart_send_data(UART_ANSR_WIFI_INIT, esp32_wifi_dhcp_start(), NULL, 0);
    return 0;
}

int uart_wifi_static_ip(char *buffer)
{
    esp32_uart_send_data(UART_ANSR_WIFI_INIT, esp32_wifi_static_ip((static_ip_set *)buffer), NULL, 0);
    return 0;
}

int uart_wifi_control(char *buffer)
{
    // shutdonw socket if exit
    if (net_mode == NET_SOCKET)
    {
        esp32_socket_exit();
        net_mode = -1;
    }
    else if (net_mode == NET_TLS)
    {
        esp32_tls_exit();
        net_mode = -1;
    }
    else
    {
        ESP_LOGI(WIFI_TAG, "no socket");
    }
    if (server_recv_handle != NULL)
    {
        vTaskDelete(server_recv_handle);
        server_recv_handle = NULL;
    }

    // connect or disconnect
    if (buffer[0] == 0)
    {
        esp32_uart_send_data(UART_ANSR_WIFI_CTRL, esp32_wifi_connect(), NULL, 0);
    }
    else if (buffer[0] == 1)
    {
        esp32_uart_send_data(UART_ANSR_WIFI_CTRL, esp32_wifi_disconnect(), NULL, 0);
    }
    else
    {
        esp32_uart_send_data(UART_ANSR_WIFI_CTRL, STATE_NO, NULL, 0);
        return -1;
    }

    return 0;
}

int uart_ble_init(char *buffer, char *size)
{
    // ble set
    if (ble_init_done)
    {
#if INF_LOG_ENABLE
        ESP_LOGI(UART_TAG, "BLE init already");
#endif
        return -1;
    }

    // uuid_1
    uint8_t addr_uuid_1 = 0;
    char *uuid_1_str    = &buffer[addr_uuid_1];
    uint32_t uuid_1     = strtoul(uuid_1_str, NULL, 16);
    uint8_t len_uuid_1  = strlen(uuid_1_str);

    // uuid_2
    uint8_t addr_uuid_2 = addr_uuid_1 + len_uuid_1 + 1;
    char *uuid_2_str    = &buffer[addr_uuid_2];
    uint32_t uuid_2     = strtoul(uuid_2_str, NULL, 16);
    uint8_t len_uuid_2  = strlen(uuid_2_str);

    // name
    uint8_t addr_name   = addr_uuid_2 + len_uuid_2 + 1;
    char *adv_name      = &buffer[addr_name];
    uint8_t len_name    = strlen(adv_name);

    // data
    uint8_t addr_data   = addr_name + len_name + 1;
    char *ble_data      = &buffer[addr_data];
    uint8_t len_data    = *(uint16_t *)size - addr_data - 2;

    // ble init
    esp32_ble_set_adv_uuid(uuid_1, uuid_2);
    esp32_ble_set_adv_name(adv_name, len_name);
    esp32_ble_set_scan_rsp(ble_data, len_data);
    esp32_ble_init();

#if INF_LOG_ENABLE
    ESP_LOGI(UART_TAG, "uuid_1   = %08lX", uuid_1);
    ESP_LOGI(UART_TAG, "uuid_2   = %08lX", uuid_2);
    ESP_LOGI(UART_TAG, "adv_name = %s", adv_name);
#endif

    ble_init_done = 1;

    esp32_uart_send_data(UART_ANSR_BLE_SET, STATE_OK, NULL, 0);
#if INF_LOG_ENABLE
    ESP_LOGI(UART_TAG, "BLE startup complete!");
#endif
    return 0;
}

int esp32_get_wifi_state()
{
    int err = 0;
    err     = get_wifi_state();
    esp32_uart_send_data(UART_ANSR_WIFI_STATE, (uint8_t)err, NULL, 0);
    return err;
}

int uart_data_handle(char *buffer)
{
    switch (buffer[BUF_FLAG])
    {
    case UART_RECV_BLE_STATE:
    case UART_RECV_WIFI_STATE:
    case UART_RECV_SERVER_STATE:
    case UART_RECV_DATA_ANSR:
#if NEED_CONFIRM
        xQueueSend(recv_handle_queue, &buffer[BUF_DATA], portMAX_DELAY);
#endif
        break;
    case UART_SEND_DATA_READ:
        uart_send_data_to_wireless(&buffer[BUF_DATA], &buffer[BUF_SIZE]);
        break;
    case UART_READ_BLE_SET:
        uart_ble_init(&buffer[BUF_DATA], &buffer[BUF_SIZE]);
        break;
    case UART_READ_WIFI_INIT:
        uart_wifi_init();
        break;
    case UART_READ_DHCP_START:
        uart_wifi_dhcp_start();
        break;
    case UART_READ_WIFI_STATIC_IP:
        uart_wifi_static_ip(&buffer[BUF_DATA]);
        break;
    case UART_READ_WIFI_CTRL:
        uart_wifi_control(&buffer[BUF_DATA]);
        break;
    case UART_READ_WIFI_SET:
        uart_wifi_set(&buffer[BUF_DATA]);
        break;
    case UART_READ_SERVER_SET:
        uart_server_set(&buffer[BUF_DATA], &buffer[BUF_SIZE]);
        break;
    case UART_READ_WIFI_STATE:
        esp32_get_wifi_state();
        break;
    case UART_BLE_TEST_OPEN:
        esp32_ble_test_mode();
        ble_init_done = true;
        break;
    case UART_BLE_TEST_SEND:
        esp32_gatt_send_data(&buffer[BUF_DATA + 1], *(uint16_t *)&buffer[BUF_SIZE] - 3);
        break;
    case UART_TEST:
        esp32_uart_send_data(UART_TEST, 0, NULL, 0);
        break;
    default:
        break;
    }

    return 0;
}

int uart_read_data()
{
    static size_t len_all = 0;
    do
    {
        len_all = 0;
        uart_read_bytes(ESP32_UART_POT, buf_read, 1, 0 / portTICK_PERIOD_MS);
        do
        {
            if (buf_read[0] == (UART_HEAD & 0xff))
            {
                uart_read_bytes(ESP32_UART_POT, &buf_read[1], 1, 0 / portTICK_PERIOD_MS);
                if (buf_read[1] == ((UART_HEAD & 0xff00) >> 8))
                {
                    uart_read_bytes(ESP32_UART_POT, &buf_read[2], 2, 0 / portTICK_PERIOD_MS);
                    size_t len = *(uint16_t *)&buf_read[2];
#if RECV_DEBUG_ENABLE
                    ESP_LOGW(UART_TAG, "RECV-DEBUG: frame header matched, len=%u", (unsigned)len);
#endif
                    if (len > 0)
                    {
                        size_t length = 0;
                        length        = uart_read_bytes(ESP32_UART_POT, &buf_read[4], len, 5000 / portTICK_PERIOD_MS);

                        if (length < len)
                        {
                            ESP_LOGE(UART_TAG, "Recv data from GD32 error: timeout");
                            memset(buf_read, 0, len + 4);
                            break;
                        }
                        else
                        {
#if RECV_DEBUG_ENABLE
                            ESP_LOGW(UART_TAG, "RECV-DEBUG: got full frame, cmd=%u len=%u", buf_read[4], (unsigned)len);
#endif
#if INF_LOG_ENABLE
#if DATA_LOG_ENABLE
                            ESP_LOGI(UART_TAG, "Recv data from GD32, length = %u", len);
#endif
#if HEX_LOG_ENABLE
                            ESP_LOG_BUFFER_HEXDUMP(UART_TAG, buf_read, len + 4, ESP_LOG_WARN);
#endif
#endif
#if CHECK_ENABLED
                            uint8_t check = uart_check_set(buf_read, len + 4);
                            if (check == buf_read[len + 3])
                            {
#endif
                                uart_data_handle(buf_read);
#if CHECK_ENABLED
                            }
                            else
                            {
#if RECV_DEBUG_ENABLE
                                ESP_LOGW(UART_TAG, "RECV-DEBUG: check failed, discard");
#endif
                                // esp32_uart_send_data(data_err, CHECK_ERR, NULL, 0);
                            }
#endif
                        }
                    }
                    memset(buf_read, 0, len + 4);
                    break;
                }
                else
                {
                    buf_read[0] = buf_read[1];
                    continue;
                }
            }
            else
            {
                break;
            }
        } while (true);
        uart_get_buffered_data_len(ESP32_UART_POT, &len_all);
    } while (len_all > 0);
    return 0;
}

void uart_read_task(void *args)
{
    uart_event_t event;
    ESP_LOGI(UART_TAG, "uart_read_task started");
    for (;;)
    {
        if (xQueueReceive(uart_queue, (void *)&event, portMAX_DELAY))
        {
#if RECV_DEBUG_ENABLE
            ESP_LOGW(UART_TAG, "RECV-DEBUG: uart event type=%d size=%d", event.type, event.size);
#endif
            switch (event.type)
            {
            case UART_DATA:
                uart_read_data();
                break;
            default:
                break;
            }
        }
    }
}

int esp32_uart_init()
{
    uart_config_t uart_config = {
        .baud_rate  = 115200,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    memset(buf_read, 0, BUF_FULLSIZE);
    uart_param_config(ESP32_UART_POT, &uart_config);
    uart_set_pin(ESP32_UART_POT, ESP32_UART_TXD, ESP32_UART_RXD, ESP32_UART_RTS, ESP32_UART_CTS);
    uart_driver_install(ESP32_UART_POT, BUF_FULLSIZE, BUF_FULLSIZE, 20, &uart_queue, 0);

    return 0;
}

void esp32_uart_start(void)
{
    esp_event_loop_create_default();
    esp32_uart_init();

#if NEED_CONFIRM
    recv_handle_queue = xQueueCreate(8, sizeof(uint8_t));
#endif
    send_buffer_queue = xQueueCreate(32, sizeof(send_buffer));
    server_set_queue  = xQueueCreate(4, sizeof(server_info));

    xTaskCreate(uart_send_task, "uart_send", 1024 * 8, NULL, 2, NULL);
    xTaskCreate(uart_read_task, "uart_read", 1024 * 8, NULL, 2, NULL);
    xTaskCreate(server_connect, "server", 1024 * 16, NULL, 2, NULL);

    esp32_uart_send_data(UART_SEND_BOOT_DONE, 0, NULL, 0);
}