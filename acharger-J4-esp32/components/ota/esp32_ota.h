#pragma once

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"

/// @brief OTA初始化
/// @param  void
/// @return 成功返回0，失败返回-1
int esp32_ota_init(void);

/// @brief OTA写入数据
/// @param buffer 数据指针
/// @param size 缓存长度
/// @return 成功返回0，失败返回-1
int esp32_ota_write(char *buffer, size_t size);

/// @brief OTA完成
/// @param  void
/// @return 成功返回0，失败返回-1
int esp32_ota_stop(void);