#include "esp32_ota.h"

#define OTA_TAG "OTA"
esp_ota_handle_t update_handle = 0;
const esp_partition_t *update_partition;

int esp32_ota_init(void)
{
    update_partition = esp_ota_get_next_update_partition(NULL);
    esp_err_t err    = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &update_handle);

    if (err != ESP_OK)
    {
        ESP_LOGE(OTA_TAG, "esp_ota_begin failed (%s)", esp_err_to_name(err));
        esp_ota_abort(update_handle);
        return err;
    }
    ESP_LOGI(OTA_TAG, "esp_ota_begin succeeded");
    return err;
}

int esp32_ota_write(char *buffer, size_t size)
{
    if (update_handle == 0)
    {
        ESP_LOGE(OTA_TAG, "OTA not init yet!");
        return -1;
    }
    esp_err_t err = esp_ota_write(update_handle, buffer, size);
    if (err != ESP_OK)
    {
        esp_ota_abort(update_handle);
        return err;
    }
    ESP_LOGD(OTA_TAG, "Written image length %u", size);
    return err;
}

int esp32_ota_stop(void)
{
    if (update_handle == 0)
    {
        ESP_LOGE(OTA_TAG, "OTA not init yet!");
        return -1;
    }
    esp_err_t err = esp_ota_end(update_handle);
    if (err != ESP_OK)
    {
        if (err == ESP_ERR_OTA_VALIDATE_FAILED)
        {
            ESP_LOGE(OTA_TAG, "Image validation failed, image is corrupted");
            return err;
        }
        else
        {
            ESP_LOGE(OTA_TAG, "esp_ota_end failed (%s)!", esp_err_to_name(err));
            return err;
        }
    }
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK)
    {
        ESP_LOGE(OTA_TAG, "esp_ota_set_boot_partition failed (%s)!", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(OTA_TAG, "OTA sucess!Prepare to restart system!");
    esp_restart();
    return err;
}