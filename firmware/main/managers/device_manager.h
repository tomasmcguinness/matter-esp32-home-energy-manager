#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t device_manager_init(void);
uint64_t  device_manager_next_node_id(void);

esp_err_t device_manager_add_device(uint64_t node_id);
esp_err_t device_manager_set_vendor_name(uint64_t node_id, const char *name, size_t len);
esp_err_t device_manager_set_product_name(uint64_t node_id, const char *name, size_t len);

esp_err_t device_manager_add_endpoint(uint64_t node_id, uint16_t endpoint_id);
esp_err_t device_manager_add_device_type(uint64_t node_id, uint16_t endpoint_id, uint32_t device_type_id);
esp_err_t device_manager_set_endpoint_label(uint64_t node_id, uint16_t endpoint_id, const char *label, size_t len);
esp_err_t device_manager_set_endpoint_included(uint64_t node_id, uint16_t endpoint_id, bool included);

char     *device_manager_get_all_json(void); // caller must free
size_t    device_manager_get_electrical_sensor_endpoints(uint64_t *node_ids, uint16_t *endpoint_ids, size_t max);
esp_err_t device_manager_persist(void);
esp_err_t device_manager_clear(void);
esp_err_t device_manager_clear_device_endpoints(uint64_t node_id);
void      device_manager_log_structure(uint64_t node_id);

#ifdef __cplusplus
}
#endif
