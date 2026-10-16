#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include <stdlib.h>
#include <string.h>

#include "lwip/dns.h"
#include "lwip/err.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"

#include "esp_crt_bundle.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/esp_debug.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/platform.h"
#include "mbedtls/ssl.h"

#include "esp32_tls.h"

static const char *TLS_TAG = "TLS";

mbedtls_entropy_context entropy;
mbedtls_ctr_drbg_context ctr_drbg;
mbedtls_ssl_context ssl;
mbedtls_x509_crt cacert;
mbedtls_ssl_config conf;
mbedtls_net_context server_fd;

int esp32_tls_exit()
{
    // 断开 TLS 连接
    mbedtls_ssl_close_notify(&ssl);

    // mbedtls_ssl_session_reset(&ssl);
    mbedtls_entropy_free(&entropy);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_ssl_free(&ssl);
    mbedtls_x509_crt_free(&cacert);
    mbedtls_ssl_config_free(&conf);
    mbedtls_net_free(&server_fd);

    ESP_LOGI(TLS_TAG, "clear done");

    return 0;
}

int esp32_tls_init() { return 0; }

int esp32_tls_connect(char *server, uint16_t port)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());

    int ret;
    mbedtls_ssl_init(&ssl);
    mbedtls_x509_crt_init(&cacert);
    mbedtls_ctr_drbg_init(&ctr_drbg);
    ESP_LOGI(TLS_TAG, "Seeding the random number generator");

    mbedtls_ssl_config_init(&conf);

    mbedtls_entropy_init(&entropy);
    if ((ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, NULL, 0)) != 0)
    {
        ESP_LOGE(TLS_TAG, "mbedtls_ctr_drbg_seed returned %d", ret);
        esp32_tls_exit();
        return -1;
    }

    ESP_LOGI(TLS_TAG, "Attaching the certificate bundle...");

    ret = esp_crt_bundle_attach(&conf);

    if (ret < 0)
    {
        ESP_LOGE(TLS_TAG, "esp_crt_bundle_attach returned -0x%x\n\n", -ret);
        esp32_tls_exit();
        return -1;
    }

    ESP_LOGI(TLS_TAG, "Setting hostname for TLS session...");

    /* Hostname set here should match CN in server certificate */
    if ((ret = mbedtls_ssl_set_hostname(&ssl, server)) != 0)
    {
        ESP_LOGE(TLS_TAG, "mbedtls_ssl_set_hostname returned -0x%x", -ret);
        esp32_tls_exit();
        return -1;
    }

    ESP_LOGI(TLS_TAG, "Setting up the SSL/TLS structure...");

    if ((ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                           MBEDTLS_SSL_PRESET_DEFAULT)) != 0)
    {
        ESP_LOGE(TLS_TAG, "mbedtls_ssl_config_defaults returned %d", ret);
        esp32_tls_exit();
        return -1;
    }

    /* MBEDTLS_SSL_VERIFY_OPTIONAL is bad for security, in this example it will print
       a warning if CA verification fails but it will continue to connect.

       You should consider using MBEDTLS_SSL_VERIFY_REQUIRED in your own code.
    */
    mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&conf, &cacert, NULL);
    mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);

    if ((ret = mbedtls_ssl_setup(&ssl, &conf)) != 0)
    {
        ESP_LOGE(TLS_TAG, "mbedtls_ssl_setup returned -0x%x\n\n", -ret);
        esp32_tls_exit();
        return -1;
    }

    char port_s[6];
    itoa(port, port_s, 10);

    mbedtls_net_init(&server_fd);

    ESP_LOGI(TLS_TAG, "Connecting to %s:%s...", server, port_s);

    if ((ret = mbedtls_net_connect(&server_fd, server, port_s, MBEDTLS_NET_PROTO_TCP)) != 0)
    {
        ESP_LOGE(TLS_TAG, "mbedtls_net_connect returned -%x", -ret);
        esp32_tls_exit();
        return -1;
    }

    ESP_LOGI(TLS_TAG, "Connected.");

    mbedtls_ssl_set_bio(&ssl, &server_fd, mbedtls_net_send, mbedtls_net_recv, NULL);

    ESP_LOGI(TLS_TAG, "Performing the SSL/TLS handshake...");

    while ((ret = mbedtls_ssl_handshake(&ssl)) != 0)
    {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            ESP_LOGE(TLS_TAG, "mbedtls_ssl_handshake returned -0x%x", -ret);
            esp32_tls_exit();
            return -1;
        }
    }

    ESP_LOGI(TLS_TAG, "Verifying peer X.509 certificate...");

    int flags;
    char buf[512];
    if ((flags = mbedtls_ssl_get_verify_result(&ssl)) != 0)
    {
        /* In real life, we probably want to close connection if ret != 0 */
        ESP_LOGW(TLS_TAG, "Failed to verify peer certificate!");
        bzero(buf, sizeof(buf));
        mbedtls_x509_crt_verify_info(buf, sizeof(buf), "  ! ", flags);
        ESP_LOGW(TLS_TAG, "verification info: %s", buf);
    }
    else
    {
        ESP_LOGI(TLS_TAG, "Certificate verified.");
    }

    ESP_LOGI(TLS_TAG, "Cipher suite is %s", mbedtls_ssl_get_ciphersuite(&ssl));

    return 0;
}

int esp32_tls_send_data(char *buf_send, uint16_t len)
{
    ESP_LOGI(TLS_TAG, "Writing HTTP request...");
    int ret;

    size_t written_bytes = 0;
    do
    {
        ret = mbedtls_ssl_write(&ssl, (const unsigned char *)buf_send + written_bytes, len - written_bytes);
        if (ret >= 0)
        {
            ESP_LOGI(TLS_TAG, "%d bytes written", ret);
            written_bytes += ret;
        }
        else if (ret != MBEDTLS_ERR_SSL_WANT_WRITE && ret != MBEDTLS_ERR_SSL_WANT_READ)
        {
            ESP_LOGE(TLS_TAG, "mbedtls_ssl_write returned -0x%x", -ret);
            esp32_tls_exit();
            return -1;
        }
    } while (written_bytes < len);

    return 0;
}

int esp32_tls_recv_data(char *buf_read, uint16_t len)
{
    ESP_LOGI(TLS_TAG, "Reading HTTP response...");
    int ret;

    do
    {
        bzero(buf_read, len);
        ret = mbedtls_ssl_read(&ssl, (unsigned char *)buf_read, len);

        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE)
            continue;

        if (ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
        {
            ret = 0;
            esp32_tls_exit();
            return -1;
        }

        if (ret < 0)
        {
            ESP_LOGE(TLS_TAG, "mbedtls_ssl_read returned -0x%x", -ret);
            esp32_tls_exit();
            return -1;
        }

        if (ret == 0)
        {
            ESP_LOGI(TLS_TAG, "connection closed");
            esp32_tls_exit();
            return -1;
        }

        if (ret > 0)
        {
            return ret;
        }
    } while (1);
}
