#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t  node_manager_init(void);
esp_err_t  node_manager_upsert(const char *node_id, float x, float y);
esp_err_t  node_manager_update_settings(const char *node_id, const char *settings_json);
char      *node_manager_get_all_json(void); // caller must free
esp_err_t  node_manager_persist(void);

#ifdef __cplusplus
}
#endif
