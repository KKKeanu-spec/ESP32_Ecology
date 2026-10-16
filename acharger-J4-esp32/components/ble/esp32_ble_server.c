#include "esp32_ble_server.h"
#include "ble_config.h"
#include "ble_app.h"
#include "driver/uart.h"
#include "esp32_uart.h"
#include "esp32_wifi.h"
#include "esp_bt.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "inttypes.h"
#include "nvs_flash.h"
#include "string.h"

#define GATTS_LOG                         "GATT"

#define ESP_SPP_APP_ID                    0x56
#define SPP_SVC_INST_ID                   0
#define EXT_ADV_HANDLE                    0

#define adv_config_flag                   (1 << 0)
#define scan_rsp_config_flag              (1 << 1)

/// Characteristic UUID
#define ESP_GATT_UUID_SPP_DATA_RECEIVE    0xABF1
#define ESP_GATT_UUID_SPP_DATA_NOTIFY     0xABF2
#define ESP_GATT_UUID_SPP_COMMAND_RECEIVE 0xABF3
#define ESP_GATT_UUID_SPP_COMMAND_NOTIFY  0xABF4

enum
{
    SPP_PROFILE_APP_IDX,
    SPP_PROFILE_NUM
};
bool test_mode                         = false;
char *test_name                        = "ESP32_BLE_TEST";
static const uint8_t ble_test_data[23] = {
    /* Flags */
    0x02, 0x01, 0x06,
    /* Complete List of 16-bit Service Class UUIDs */
    0x03, 0x03, 0xCD, 0xAB,
    /* Complete Local Name in advertising */
    0x0F, 0x09, 'E', 'S', 'P', '3', '2', '_', 'B', 'L', 'E', '_', 'T', 'E', 'S', 'T'};

char *device_name;
/// SPP Service
uint32_t spp_uuid_1;
uint32_t spp_uuid_2;

static uint8_t adv_config_done = 0;

/// SPP adv data
uint32_t len_spp_adv_data;
ble_adv_data_t ble_adv_data;
uint32_t len_spp_scan_rsp;
ble_scan_rsp_t ble_scan_rsp;

static uint16_t spp_mtu_size        = 517;
static uint16_t spp_conn_id         = 0xffff;
static esp_gatt_if_t spp_gatts_if   = 0xff;
QueueHandle_t spp_uart_queue        = NULL;

static bool enable_data_ntf         = false;
static bool is_connected            = false;
static esp_bd_addr_t spp_remote_bda = {
    0x0,
};

static uint16_t spp_handle_table[SPP_IDX_NB];

static esp_ble_adv_params_t adv_params = {
    .adv_int_min       = 0x20,
    .adv_int_max       = 0x40,
    .adv_type          = ADV_TYPE_IND,
    .own_addr_type     = BLE_ADDR_TYPE_PUBLIC,
    .channel_map       = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

struct gatts_profile_inst
{
    esp_gatts_cb_t gatts_cb;
    uint16_t gatts_if;
    uint16_t app_id;
    uint16_t conn_id;
    uint16_t service_handle;
    esp_gatt_srvc_id_t service_id;
    uint16_t char_handle;
    esp_bt_uuid_t char_uuid;
    esp_gatt_perm_t perm;
    esp_gatt_char_prop_t property;
    uint16_t descr_handle;
    esp_bt_uuid_t descr_uuid;
};

typedef struct spp_receive_data_node
{
    int32_t len;
    uint8_t *node_buff;
    struct spp_receive_data_node *next_node;
} spp_receive_data_node_t;

static spp_receive_data_node_t *temp_spp_recv_data_node_p1 = NULL;
static spp_receive_data_node_t *temp_spp_recv_data_node_p2 = NULL;

typedef struct spp_receive_data_buff
{
    int32_t node_num;
    int32_t buff_size;
    spp_receive_data_node_t *first_node;
} spp_receive_data_buff_t;

static spp_receive_data_buff_t SppRecvDataBuff = {.node_num = 0, .buff_size = 0, .first_node = NULL};

static void gatts_profile_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                                        esp_ble_gatts_cb_param_t *param);

/* One gatt-based profile one app_id and one gatts_if, this array will store the gatts_if returned by ESP_GATTS_REG_EVT
 */
static struct gatts_profile_inst spp_profile_tab[SPP_PROFILE_NUM] = {
    [SPP_PROFILE_APP_IDX] =
        {
            .gatts_cb = gatts_profile_event_handler,
            .gatts_if = ESP_GATT_IF_NONE, /* Not get the gatt_if, so initial is ESP_GATT_IF_NONE */
        },
};

/*
 *  SPP PROFILE ATTRIBUTES
 ****************************************************************************************
 */

#define CHAR_DECLARATION_SIZE (sizeof(uint8_t))
static const uint16_t primary_service_uuid         = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t character_declaration_uuid   = ESP_GATT_UUID_CHAR_DECLARE;
static const uint16_t character_client_config_uuid = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;

static const uint8_t char_prop_read_notify         = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static const uint8_t char_prop_read_write          = ESP_GATT_CHAR_PROP_BIT_WRITE_NR | ESP_GATT_CHAR_PROP_BIT_READ;

/// SPP Service - data receive characteristic, read&write without response
static const uint16_t spp_data_receive_uuid        = ESP_GATT_UUID_SPP_DATA_RECEIVE;
static const uint8_t spp_data_receive_val[20]      = {0x00};

/// SPP Service - data notify characteristic, notify&read
static const uint16_t spp_data_notify_uuid         = ESP_GATT_UUID_SPP_DATA_NOTIFY;
static const uint8_t spp_data_notify_val[20]       = {0x00};
static const uint8_t spp_data_notify_ccc[2]        = {0x00, 0x00};

/// SPP Service - command characteristic, read&write without response
static const uint16_t spp_command_uuid             = ESP_GATT_UUID_SPP_COMMAND_RECEIVE;
static const uint8_t spp_command_val[10]           = {0x00};

/// SPP Service - status characteristic, notify&read
static const uint16_t spp_status_uuid              = ESP_GATT_UUID_SPP_COMMAND_NOTIFY;
static const uint8_t spp_status_val[10]            = {0x00};
static const uint8_t spp_status_ccc[2]             = {0x00, 0x00};

/// Full HRS Database Description - Used to add attributes into the database
static const esp_gatts_attr_db_t spp_gatt_db[SPP_IDX_NB] = {
    // SPP -  Service Declaration
    [SPP_IDX_SVC]                  = {{ESP_GATT_AUTO_RSP},
                                      {ESP_UUID_LEN_16, (uint8_t *)&primary_service_uuid, ESP_GATT_PERM_READ, sizeof(spp_uuid_1),
                                       sizeof(spp_uuid_1), (uint8_t *)&spp_uuid_1}},

    // SPP -  data receive characteristic Declaration
    [SPP_IDX_SPP_DATA_RECV_CHAR]   = {{ESP_GATT_AUTO_RSP},
                                      {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
                                       CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_read_write}},

    // SPP -  data receive characteristic Value
    [SPP_IDX_SPP_DATA_RECV_VAL]    = {{ESP_GATT_AUTO_RSP},
                                      {ESP_UUID_LEN_16, (uint8_t *)&spp_data_receive_uuid,
                                       ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, SPP_DATA_MAX_LEN,
                                       sizeof(spp_data_receive_val), (uint8_t *)spp_data_receive_val}},

    // SPP -  data notify characteristic Declaration
    [SPP_IDX_SPP_DATA_NOTIFY_CHAR] = {{ESP_GATT_AUTO_RSP},
                                      {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
                                       CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE,
                                       (uint8_t *)&char_prop_read_notify}},

    // SPP -  data notify characteristic Value
    [SPP_IDX_SPP_DATA_NTY_VAL]     = {{ESP_GATT_AUTO_RSP},
                                      {ESP_UUID_LEN_16, (uint8_t *)&spp_data_notify_uuid, ESP_GATT_PERM_READ,
                                       SPP_DATA_MAX_LEN, sizeof(spp_data_notify_val), (uint8_t *)spp_data_notify_val}},

    // SPP -  data notify characteristic - Client Characteristic Configuration Descriptor
    [SPP_IDX_SPP_DATA_NTF_CFG]     = {{ESP_GATT_AUTO_RSP},
                                      {ESP_UUID_LEN_16, (uint8_t *)&character_client_config_uuid,
                                       ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, sizeof(uint16_t),
                                       sizeof(spp_data_notify_ccc), (uint8_t *)spp_data_notify_ccc}},

    // SPP -  command characteristic Declaration
    [SPP_IDX_SPP_COMMAND_CHAR]     = {{ESP_GATT_AUTO_RSP},
                                      {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
                                       CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_read_write}},

    // SPP -  command characteristic Value
    [SPP_IDX_SPP_COMMAND_VAL]      = {{ESP_GATT_AUTO_RSP},
                                      {ESP_UUID_LEN_16, (uint8_t *)&spp_command_uuid,
                                       ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, SPP_CMD_MAX_LEN, sizeof(spp_command_val),
                                       (uint8_t *)spp_command_val}},

    // SPP -  status characteristic Declaration
    [SPP_IDX_SPP_STATUS_CHAR]      = {{ESP_GATT_AUTO_RSP},
                                      {ESP_UUID_LEN_16, (uint8_t *)&character_declaration_uuid, ESP_GATT_PERM_READ,
                                       CHAR_DECLARATION_SIZE, CHAR_DECLARATION_SIZE, (uint8_t *)&char_prop_read_notify}},

    // SPP -  status characteristic Value
    [SPP_IDX_SPP_STATUS_VAL]       = {{ESP_GATT_AUTO_RSP},
                                      {ESP_UUID_LEN_16, (uint8_t *)&spp_status_uuid, ESP_GATT_PERM_READ, SPP_STATUS_MAX_LEN,
                                       sizeof(spp_status_val), (uint8_t *)spp_status_val}},

    // SPP -  status characteristic - Client Characteristic Configuration Descriptor
    [SPP_IDX_SPP_STATUS_CFG]       = {{ESP_GATT_AUTO_RSP},
                                      {ESP_UUID_LEN_16, (uint8_t *)&character_client_config_uuid,
                                       ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, sizeof(uint16_t), sizeof(spp_status_ccc),
                                       (uint8_t *)spp_status_ccc}},

};

static uint8_t find_char_and_desr_index(uint16_t handle)
{
    uint8_t error = 0xff;

    for (int i = 0; i < SPP_IDX_NB; i++)
    {
        if (handle == spp_handle_table[i])
        {
            return i;
        }
    }

    return error;
}

static bool store_wr_buffer(esp_ble_gatts_cb_param_t *p_data)
{
    temp_spp_recv_data_node_p1 = (spp_receive_data_node_t *)malloc(sizeof(spp_receive_data_node_t));

    if (temp_spp_recv_data_node_p1 == NULL)
    {
        ESP_LOGI(GATTS_LOG, "malloc error %s %d\n", __func__, __LINE__);
        return false;
    }
    if (temp_spp_recv_data_node_p2 != NULL)
    {
        temp_spp_recv_data_node_p2->next_node = temp_spp_recv_data_node_p1;
    }
    temp_spp_recv_data_node_p1->len = p_data->write.len;
    SppRecvDataBuff.buff_size += p_data->write.len;
    temp_spp_recv_data_node_p1->next_node = NULL;
    temp_spp_recv_data_node_p1->node_buff = (uint8_t *)malloc(p_data->write.len);
    temp_spp_recv_data_node_p2            = temp_spp_recv_data_node_p1;
    memcpy(temp_spp_recv_data_node_p1->node_buff, p_data->write.value, p_data->write.len);
    if (SppRecvDataBuff.node_num == 0)
    {
        SppRecvDataBuff.first_node = temp_spp_recv_data_node_p1;
        SppRecvDataBuff.node_num++;
    }
    else
    {
        SppRecvDataBuff.node_num++;
    }

    return true;
}

static void free_write_buffer(void)
{
    temp_spp_recv_data_node_p1 = SppRecvDataBuff.first_node;

    while (temp_spp_recv_data_node_p1 != NULL)
    {
        temp_spp_recv_data_node_p2 = temp_spp_recv_data_node_p1->next_node;
        free(temp_spp_recv_data_node_p1->node_buff);
        free(temp_spp_recv_data_node_p1);
        temp_spp_recv_data_node_p1 = temp_spp_recv_data_node_p2;
    }

    SppRecvDataBuff.node_num   = 0;
    SppRecvDataBuff.buff_size  = 0;
    SppRecvDataBuff.first_node = NULL;
}

static void print_write_buffer(void)
{
    temp_spp_recv_data_node_p1 = SppRecvDataBuff.first_node;

    while (temp_spp_recv_data_node_p1 != NULL)
    {
#if INF_LOG_ENABLE
        ESP_LOGI(GATTS_LOG, "Recv data from BLE, length = %ld", temp_spp_recv_data_node_p1->len);
#if HEX_LOG_ENABLE
        ESP_LOG_BUFFER_HEXDUMP("gatt_recv_buffer", (char *)(temp_spp_recv_data_node_p1->node_buff),
                               temp_spp_recv_data_node_p1->len, ESP_LOG_INFO);
#endif
#endif
        /* 主机模式: 分片数据本地处理，不再转发给 GD32/CCU */
        ble_app_handle_data(temp_spp_recv_data_node_p1->node_buff,
                            (uint16_t)temp_spp_recv_data_node_p1->len);
        temp_spp_recv_data_node_p1 = temp_spp_recv_data_node_p1->next_node;
    }
}

int esp32_gatt_send_data(void *data, uint16_t size)
{
    uint8_t total_num           = 0;
    uint8_t current_num         = 0;
    int err                     = 0;
    static uint8_t *ntf_value_p = NULL;

    if (size <= (spp_mtu_size - 3))
    {
        err = esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, spp_handle_table[SPP_IDX_SPP_DATA_NTY_VAL], size,
                                          data, false);
    }
    else if (size > (spp_mtu_size - 3))
    {
        if ((size % (spp_mtu_size - 7)) == 0)
        {
            total_num = size / (spp_mtu_size - 7);
        }
        else
        {
            total_num = size / (spp_mtu_size - 7) + 1;
        }
        current_num = 1;
        ntf_value_p = (uint8_t *)malloc((spp_mtu_size - 3) * sizeof(uint8_t));
        if (ntf_value_p == NULL)
        {
            ESP_LOGE(GATTS_LOG, "%s malloc.2 failed\n", __func__);
            /* 注意: 此处不再 free(data)。
             * 原实现对 data 调用 free()，但短数据分支并不 free，
             * 语义矛盾且会误释放调用方传入的栈内存。调用方负责释放自己分配的内存。 */
            return -1;
        }
        while (current_num <= total_num)
        {
            if (current_num < total_num)
            {
                ntf_value_p[0] = '#';
                ntf_value_p[1] = '#';
                ntf_value_p[2] = total_num;
                ntf_value_p[3] = current_num;
                memcpy(ntf_value_p + 4, data + (current_num - 1) * (spp_mtu_size - 7), (spp_mtu_size - 7));
                err = esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, spp_handle_table[SPP_IDX_SPP_DATA_NTY_VAL],
                                                  (spp_mtu_size - 3), ntf_value_p, false);
            }
            else if (current_num == total_num)
            {
                ntf_value_p[0] = '#';
                ntf_value_p[1] = '#';
                ntf_value_p[2] = total_num;
                ntf_value_p[3] = current_num;
                memcpy(ntf_value_p + 4, data + (current_num - 1) * (spp_mtu_size - 7),
                       (size - (current_num - 1) * (spp_mtu_size - 7)));
                err = esp_ble_gatts_send_indicate(spp_gatts_if, spp_conn_id, spp_handle_table[SPP_IDX_SPP_DATA_NTY_VAL],
                                                  (size - (current_num - 1) * (spp_mtu_size - 7) + 4), ntf_value_p,
                                                  false);
            }
            vTaskDelay(20 / portTICK_PERIOD_MS);
            current_num++;
        }
    }

    return err;
}

bool esp32_ble_is_connected(void)
{
    return is_connected;
}

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    esp_err_t err;
    ESP_LOGE(GATTS_LOG, "GAP_EVT, event %d\n", event);

    switch (event)
    {
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        if (test_mode)
        {
            esp_ble_gap_start_advertising(&adv_params);
        }
        else
        {
            adv_config_done &= (~adv_config_flag);
            if (adv_config_done == 0)
            {
                esp_ble_gap_start_advertising(&adv_params);
            }
        }
        break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
        adv_config_done &= (~scan_rsp_config_flag);
        if (adv_config_done == 0)
        {
            esp_ble_gap_start_advertising(&adv_params);
        }
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        // advertising start complete event to indicate advertising start successfully or failed
        if ((err = param->adv_start_cmpl.status) != ESP_BT_STATUS_SUCCESS)
        {
            ESP_LOGE(GATTS_LOG, "Advertising start failed: %s\n", esp_err_to_name(err));
        }
        break;
    default:
        break;
    }
}

static void gatts_profile_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                                        esp_ble_gatts_cb_param_t *param)
{
    esp_ble_gatts_cb_param_t *p_data = (esp_ble_gatts_cb_param_t *)param;
    uint8_t res                      = 0xff;

    ESP_LOGI(GATTS_LOG, "event = %x\n", event);
    switch (event)
    {
    case ESP_GATTS_REG_EVT:
        ESP_LOGI(GATTS_LOG, "%s %d\n", __func__, __LINE__);
        esp_ble_gap_set_device_name(device_name);

        ESP_LOGI(GATTS_LOG, "%s %d\n", __func__, __LINE__);
        if (test_mode)
        {
            ESP_LOGI(GATTS_LOG, "%s %d\n", __func__, __LINE__);
            esp_ble_gap_config_adv_data_raw((uint8_t *)&ble_test_data, 23);
        }
        else
        {
            ESP_LOGI(GATTS_LOG, "%s %d\n", __func__, __LINE__);
            esp_ble_gap_config_adv_data_raw((uint8_t *)&ble_adv_data, len_spp_adv_data);
            adv_config_done |= adv_config_flag;

            ESP_LOGI(GATTS_LOG, "%s %d\n", __func__, __LINE__);
            esp_ble_gap_config_scan_rsp_data_raw((uint8_t *)&ble_scan_rsp, len_spp_scan_rsp);
            adv_config_done |= scan_rsp_config_flag;
        }
        ESP_LOGI(GATTS_LOG, "%s %d\n", __func__, __LINE__);
        esp_ble_gatts_create_attr_tab(spp_gatt_db, gatts_if, SPP_IDX_NB, SPP_SVC_INST_ID);

        break;
    case ESP_GATTS_READ_EVT:
        res = find_char_and_desr_index(p_data->read.handle);
        if (res == SPP_IDX_SPP_STATUS_VAL)
        {
            // TODO:client read the status characteristic
        }
        break;
    case ESP_GATTS_WRITE_EVT:
    {
        res = find_char_and_desr_index(p_data->write.handle);
        if (p_data->write.is_prep == false)
        {
            ESP_LOGI(GATTS_LOG, "ESP_GATTS_WRITE_EVT : handle = %d\n", res);
            if (res == SPP_IDX_SPP_COMMAND_VAL)
            {
                ESP_LOGI(GATTS_LOG, "Recv cmd from BLE(0xABF3), length = %d", p_data->write.len);
                ESP_LOG_BUFFER_HEXDUMP(GATTS_LOG, (char *)(p_data->write.value), p_data->write.len, ESP_LOG_DEBUG);

                /* 主机模式: 命令通道同样交由 ESP32 自行解析。
                 * 不分配临时缓冲，直接使用写回调中的数据指针（回调返回即失效，
                 * 故同步处理），顺便消除原实现的 malloc 内存泄漏。 */
                ble_app_handle_data(p_data->write.value, p_data->write.len);
            }
            else if (res == SPP_IDX_SPP_DATA_NTF_CFG)
            {
                /* ===== 调试: 观察 APP 何时开启/关闭 0xABF2 通知订阅 ===== */
                ESP_LOGW(GATTS_LOG, ">>> [CCCD] 0xABF2 notify cfg: %02X %02X",
                         p_data->write.value[0], p_data->write.value[1]);

                if ((p_data->write.len == 2) && (p_data->write.value[0] == 0x01) && (p_data->write.value[1] == 0x00))
                {
                    enable_data_ntf = true;
                }
                else if ((p_data->write.len == 2) && (p_data->write.value[0] == 0x00) &&
                         (p_data->write.value[1] == 0x00))
                {
                    enable_data_ntf = false;
                }
            }
            else if (res == SPP_IDX_SPP_DATA_RECV_VAL)
            {
                ESP_LOGI(GATTS_LOG, "Recv data from BLE(0xABF1), length = %d", p_data->write.len);
                ESP_LOG_BUFFER_HEXDUMP(GATTS_LOG, (char *)(p_data->write.value), p_data->write.len, ESP_LOG_DEBUG);

                /* 主机模式: 不再转发给 GD32/CCU，由 ESP32 自行解析处理 */
                ble_app_handle_data(p_data->write.value, p_data->write.len);
            }
            else
            {
                ESP_LOGE(GATTS_LOG, "BLE recv error\n");
            }
        }
        else if ((p_data->write.is_prep == true) && (res == SPP_IDX_SPP_DATA_RECV_VAL))
        {
            ESP_LOGI(GATTS_LOG, "ESP_GATTS_PREP_WRITE_EVT : handle = %d\n", res);
            store_wr_buffer(p_data);
        }
        break;
    }
    case ESP_GATTS_EXEC_WRITE_EVT:
    {
        ESP_LOGI(GATTS_LOG, "ESP_GATTS_EXEC_WRITE_EVT\n");
        if (p_data->exec_write.exec_write_flag)
        {
            print_write_buffer();
            free_write_buffer();
        }
        break;
    }
    case ESP_GATTS_MTU_EVT:
        spp_mtu_size = p_data->mtu.mtu;
        ESP_LOGI(GATTS_LOG, "MTU now = %u\n", spp_mtu_size);
        break;
    case ESP_GATTS_CONF_EVT:
        break;
    case ESP_GATTS_UNREG_EVT:
        break;
    case ESP_GATTS_DELETE_EVT:
        break;
    case ESP_GATTS_START_EVT:
        break;
    case ESP_GATTS_STOP_EVT:
        break;
    case ESP_GATTS_CONNECT_EVT:
        spp_conn_id  = p_data->connect.conn_id;
        spp_gatts_if = gatts_if;
        is_connected = true;
        memcpy(&spp_remote_bda, &p_data->connect.remote_bda, sizeof(esp_bd_addr_t));
        ESP_LOGI(GATTS_LOG, "=== BLE 已连接  (conn_id=%d, addr=%02X:%02X:%02X:%02X:%02X:%02X) ===",
                 spp_conn_id,
                 spp_remote_bda[0], spp_remote_bda[1], spp_remote_bda[2],
                 spp_remote_bda[3], spp_remote_bda[4], spp_remote_bda[5]);
        if (!test_mode)
            esp32_uart_send_data(UART_SEND_BLE_STATE, STATE_OK, NULL, 0);
        break;
    case ESP_GATTS_DISCONNECT_EVT:
        is_connected    = false;
        enable_data_ntf = false;
        ESP_LOGW(GATTS_LOG, "=== BLE 已断开 (reason=0x%02X) ===", p_data->disconnect.reason);
        esp_ble_gap_start_advertising(&adv_params);
        ESP_LOGI(GATTS_LOG, "BLE 重新开始广播，等待新的连接");
        if (!test_mode)
            esp32_uart_send_data(UART_SEND_BLE_STATE, STATE_NO, NULL, 0);
        break;
    case ESP_GATTS_OPEN_EVT:
        break;
    case ESP_GATTS_CANCEL_OPEN_EVT:
        break;
    case ESP_GATTS_CLOSE_EVT:
        break;
    case ESP_GATTS_LISTEN_EVT:
        break;
    case ESP_GATTS_CONGEST_EVT:
        break;
    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
    {
        ESP_LOGI(GATTS_LOG, "The number handle =%x\n", param->add_attr_tab.num_handle);
        if (param->add_attr_tab.status != ESP_GATT_OK)
        {
            ESP_LOGE(GATTS_LOG, "Create attribute table failed, error code=0x%x", param->add_attr_tab.status);
        }
        else if (param->add_attr_tab.num_handle != SPP_IDX_NB)
        {
            ESP_LOGE(GATTS_LOG, "Create attribute table abnormally, num_handle (%d) doesn't equal to HRS_IDX_NB(%d)",
                     param->add_attr_tab.num_handle, SPP_IDX_NB);
        }
        else
        {
            memcpy(spp_handle_table, param->add_attr_tab.handles, sizeof(spp_handle_table));
            esp_ble_gatts_start_service(spp_handle_table[SPP_IDX_SVC]);
        }
        break;
    }
    default:
        break;
    }
}

static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param)
{
    ESP_LOGI(GATTS_LOG, "EVT %d, gatts if %d\n", event, gatts_if);

    /* If event is register event, store the gatts_if for each profile */
    if (event == ESP_GATTS_REG_EVT)
    {
        if (param->reg.status == ESP_GATT_OK)
        {
            spp_profile_tab[SPP_PROFILE_APP_IDX].gatts_if = gatts_if;
        }
        else
        {
            ESP_LOGI(GATTS_LOG, "Reg app failed, app_id %04x, status %d\n", param->reg.app_id, param->reg.status);
            return;
        }
    }

    do
    {
        int idx;
        for (idx = 0; idx < SPP_PROFILE_NUM; idx++)
        {
            if (gatts_if == ESP_GATT_IF_NONE || /* ESP_GATT_IF_NONE, not specify a certain gatt_if, need to call every
                                                   profile cb function */
                gatts_if == spp_profile_tab[idx].gatts_if)
            {
                if (spp_profile_tab[idx].gatts_cb)
                {
                    spp_profile_tab[idx].gatts_cb(event, gatts_if, param);
                }
            }
        }
    } while (0);
}

void esp32_ble_set_adv_name(char *name, uint8_t size)
{
    device_name            = name;
    ble_adv_data.name.size = size + sizeof(ble_adv_data.name.type);
    ble_adv_data.name.type = BLE_ADV_FULL_NAME;
    memcpy(&ble_adv_data.name.name[0], device_name, size);

    len_spp_adv_data = size + sizeof(ble_adv_data) - sizeof(ble_adv_data.name.name);
    ESP_LOGI(GATTS_LOG, "len_spp_adv_data = %" PRIu32, len_spp_adv_data);
}

void esp32_ble_set_adv_uuid(uint32_t uuid_1, uint32_t uuid_2)
{
    spp_uuid_1             = uuid_1;
    spp_uuid_2             = uuid_2;

    ble_adv_data.flag.size = sizeof(ble_adv_data.flag.type) + sizeof(ble_adv_data.flag.flag);
    ble_adv_data.flag.type = BLE_ADV_FLAG;
    ble_adv_data.flag.flag = 0b00110U;
    ble_adv_data.uuid.size =
        sizeof(ble_adv_data.uuid.type) + sizeof(ble_adv_data.uuid.uuid_1) + sizeof(ble_adv_data.uuid.uuid_2);
    ble_adv_data.uuid.type   = sizeof(uuid_1) == 4 ? BLE_ADV_COMPLETE_32_UUID : BLE_ADV_COMPLETE_16_UUID;
    ble_adv_data.uuid.uuid_1 = uuid_1;
    ble_adv_data.uuid.uuid_2 = uuid_2;
}

void esp32_ble_set_scan_rsp(char *data, uint8_t size)
{
    len_spp_scan_rsp       = size + sizeof(ble_scan_rsp.data.size) + sizeof(ble_scan_rsp.data.type);

    ble_scan_rsp.data.size = size + sizeof(ble_scan_rsp.data.type);
    ble_scan_rsp.data.type = BLE_ADV_MANUFACTURER_DATA;

    memcpy(&ble_scan_rsp.data.data[0], data, size);
}

int esp32_ble_test_mode()
{
    if (test_mode)
    {
        return -1;
    }
    else
    {
        test_mode = true;
        return esp32_ble_init();
    }
}

/*!
 * @brief 用代码内写死的参数启动 BLE（主机模式）
 *
 * 与 uart_ble_init() 的区别: 参数不来自 CCU 下发，直接取自 ble_config.h。
 * 调用顺序必须是 先设置UUID/名称/扫描响应，再 esp32_ble_init()，
 * 因为广播数据在 ESP_GATTS_REG_EVT 事件里被真正下发。
 */
int esp32_ble_start_with_config(void)
{
    static char name[]           = BLE_NAME; /* 非 const，接口要求 char* */
    static uint8_t scan_rsp[]    = BLE_SCAN_RSP_DATA;

    ESP_LOGI(GATTS_LOG, "=== BLE 主机模式启动（参数写死，不依赖 CCU） ===");
    ESP_LOGI(GATTS_LOG, "  HW  UUID : %08lX", (unsigned long)BLE_HW_UUID);
    ESP_LOGI(GATTS_LOG, "  APP UUID : %08lX", (unsigned long)BLE_APP_UUID);
    ESP_LOGI(GATTS_LOG, "  adv name : \"%s\"", name);
    ESP_LOGI(GATTS_LOG, "  scan rsp : type=0x%02X  ver=%u.%u",
             (unsigned)scan_rsp[0], (unsigned)scan_rsp[1], (unsigned)scan_rsp[2]);

    /* 顺序与 CCU 下发路径保持一致: uuid -> name -> scan_rsp -> init */
    esp32_ble_set_adv_uuid(BLE_HW_UUID, BLE_APP_UUID);
    esp32_ble_set_adv_name(name, (uint8_t)strlen(name));
    esp32_ble_set_scan_rsp((char *)scan_rsp, BLE_SCAN_RSP_LEN);

    return esp32_ble_init();
}

int esp32_ble_init()
{
    esp_err_t ret;
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();

    // Initialize NVS
    ret                               = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    ESP_ERROR_CHECK(esp_ble_gatts_register_callback(gatts_event_handler));
    ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));
    ESP_ERROR_CHECK(esp_ble_gatts_app_register(ESP_SPP_APP_ID));

    return 0;
}
