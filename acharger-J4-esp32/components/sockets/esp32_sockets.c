#include "esp32_sockets.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"
#include <netdb.h>
#include <string.h>
#include <sys/param.h>
#include <sys/socket.h>

static const char *SOCKET_TAG = "socket";
static int sock               = 0;

// socket 层收数据调试打印开关：server_recv_task 会高频循环调用 recv()，
// 该日志每次都打会淹没 OTA 关键日志。置 0 屏蔽，E 级错误打印不受影响。
#define SOCKET_LOG_ENABLE 0
struct sockaddr_in dest_addr;

int socket_set_dest_addr(struct sockaddr_in *dest_addr, char *addr, uint16_t port)
{
    esp_ip4_addr_t ip4_addr;
    const struct addrinfo hints = {
        .ai_family   = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res;

    int err = getaddrinfo(addr, NULL, &hints, &res);
    if (err != 0 || res == NULL)
    {
        ESP_LOGE(SOCKET_TAG, "DNS lookup failed err=%d res=%p", err, res);
        return -1;
    }
    memcpy(&ip4_addr, &((struct sockaddr_in *)(res->ai_addr))->sin_addr, sizeof(ip4_addr));
    ESP_LOG_BUFFER_HEXDUMP("socket", &ip4_addr.addr, sizeof(ip4_addr), ESP_LOG_INFO);
    freeaddrinfo(res);

    dest_addr->sin_addr.s_addr = ip4_addr.addr;
    dest_addr->sin_family      = AF_INET;
    dest_addr->sin_port        = htons(port);

    return 0;
}

int esp32_socket_exit()
{
    // ESP-IDF 里 fd 0/1/2 被控制台（stdin/stdout/stderr）占用，lwIP 的 socket fd 恒 > 0；
    // 且本文件 sock 初值为 0，若用 >= 0 会在首次连接时误判为"存在旧连接"而执行
    if (sock > 0)
    {
        shutdown(sock, 0);
        close(sock);
    }
    sock = -1;

    return 0;
}

int socket_set_sock(int *sock)
{
    *sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);

    return 0;
}

int esp32_socket_init()
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());

    return 0;
}

int esp32_socket_connect(char *addr, uint16_t port)
{
    socket_set_dest_addr(&dest_addr, addr, port);

    // 重建 fd 前先关闭可能遗留的旧连接，避免 errno 9 (EBADF) 竞争：
    // GD32 可能先发 WIFI_CTRL 关掉 sock，再发 SET_SERVER 触发本函数，
    // 若直接复用已被 close 的 fd 会 connect 失败。
    // 同上：保持 > 0。fd 0 属于控制台，且 sock 初值为 0；
    // 用 >= 0 会误关 fd 0（控制台）造成死机。
    if (sock > 0)
    {
        close(sock);
        sock = -1;
    }
    socket_set_sock(&sock);
    if (sock < 0)
    {
        ESP_LOGE(SOCKET_TAG, "Socket create failed");
        return -1;
    }

    int err = connect(sock, (struct sockaddr *)&dest_addr, sizeof(struct sockaddr_in));
    if (err != 0)
    {
        ESP_LOGE(SOCKET_TAG, "Socket unable to connect: errno %d", errno);
        esp32_socket_exit();
        return err;
    }

    // 设置收发超时，避免 OTA 下载时 recv/send 永久阻塞导致 ESP32 挂死。
    // 超时后 recv 返回 -1 (errno EAGAIN)，由调用方关闭连接并触发重建，
    // 而不是无限等待 GD32 硬复位。
    struct timeval timeout = { .tv_sec = 5, .tv_usec = 0 };
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) != 0)
    {
        ESP_LOGE(SOCKET_TAG, "set SO_RCVTIMEO failed: errno %d", errno);
    }
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) != 0)
    {
        ESP_LOGE(SOCKET_TAG, "set SO_SNDTIMEO failed: errno %d", errno);
    }

    ESP_LOGI(SOCKET_TAG, "Successfully connected");
    return 0;
}

int esp32_socket_send_data(char *tx_buffer, uint16_t size)
{
    int err = send(sock, tx_buffer, size, 0);
    if (err < 0)
    {
        ESP_LOGE(SOCKET_TAG, "Error occurred during sending: errno %d", errno);
    }

    return err;
}

int esp32_socket_recv_data(char *buf_recv, uint16_t size)
{
#if SOCKET_LOG_ENABLE
    ESP_LOGI(SOCKET_TAG, "Reading socket response...");
#endif
    bzero(buf_recv, size);

    int len = recv(sock, buf_recv, size, 0);
    // Error occurred during receiving
    if (len < 0)
    {
        // 空闲超时 (SO_RCVTIMEO 触发 EAGAIN/EINTR) 仅表示“当前没有数据”，
        // TCP 连接仍然有效。HTTP keep-alive 下 GD32 逐包发 Range GET、服务器逐段返回，
        // 两段响应之间 recv 必然空闲若干秒，此时绝不能关闭 socket，
        // 否则上层 server_recv_task 会误判断连、挂起自身且再也不被唤醒，
        // 导致 OTA 在“包间隙”被永久掐断（典型现象：下到某一包后 recv 超时、再也无法恢复）。
        // 仅当真正的错误 / 对端关闭（非 EAGAIN/EINTR）才关闭 fd。
        if (errno == EAGAIN || errno == EINTR)
        {
            // 返回值契约：0 仅代表“空闲超时、连接仍然有效”。
            // 对端关闭(FIN)绝不能返回 0，必须返回 -1（见下方 len == 0 分支），
            // 否则上层会把死连接当成空闲无限空转。
            return 0;
        }
        ESP_LOGE(SOCKET_TAG, "recv failed: errno %d, len = %d", errno, len);
        esp32_socket_exit();
        return -1;
    }

    // 对端关闭连接（收到 FIN）：recv 返回 0。
    // 必须与“空闲超时”严格区分——对象存储/CDN（如阿里云 OSS）会在单条 keep-alive 连接上
    // 处理若干次 Range 请求后主动关闭连接，这是正常 HTTP 语义。
    // 若此处与 EAGAIN 一样返回 0，上层 server_recv_task 会把“已死的连接”当成“暂时无数据”
    // 无限空转：不关 socket、也不通知 GD32，导致 GD32 后续的 Range GET 全部石沉大海，
    // 表现为 OTA 下到某一包后反复 recv timeout 再也无法恢复。
    // 因此真正的 FIN 必须按断连处理：关闭 fd 并让上层回报 GD32 重建连接后从当前包续传。
    if (len == 0)
    {
        ESP_LOGW(SOCKET_TAG, "peer closed connection (FIN), treat as disconnected");
        esp32_socket_exit();
        return -1;
    }

    return len;
}
