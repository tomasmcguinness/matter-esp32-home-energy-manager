#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t  node_manager_init(void);
esp_err_t  node_manager_upsert(const char *node_id, float x, float y, const char *settings_json);
esp_err_t  node_manager_update_settings(const char *node_id, const char *settings_json);
esp_err_t  node_manager_delete(const char *node_id);
esp_err_t  node_manager_upsert_edge(const char *id, const char *source, const char *target, const char *source_handle, const char *target_handle);
esp_err_t  node_manager_delete_edge(const char *id);
char      *node_manager_get_all_json(void); // caller must free
esp_err_t  node_manager_persist(void);
esp_err_t  node_manager_clear(void);

#ifdef __cplusplus
}
#endif
