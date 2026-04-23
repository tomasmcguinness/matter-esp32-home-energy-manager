#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SETTINGS_NAME_LEN 64

typedef struct {
    char name[SETTINGS_NAME_LEN];
} settings_t;

esp_err_t settings_store_init(void);
const settings_t *settings_store_get(void);
esp_err_t settings_store_update(const char *name, settings_t *out);

#ifdef __cplusplus
}
#endif
