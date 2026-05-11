#pragma once

#include "esp_err.h"

#define SD_CARD_MOUNT_POINT "/sdcard"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t sd_card_init(void);

#ifdef __cplusplus
}
#endif
