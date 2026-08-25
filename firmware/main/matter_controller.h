#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t matter_controller_start(void);
uint64_t  matter_controller_allocate_node_id(void);
esp_err_t matter_controller_commission_on_network(const char *onboarding_payload, uint64_t *node_id_out);
esp_err_t matter_controller_remove_node(uint64_t node_id);
// Drop the controller's local bookkeeping for a node (NVS node list) without
// contacting the device. Used to delete an unreachable/dead device that can no
// longer be cleanly unpaired.
void      matter_controller_forget_node(uint64_t node_id);
esp_err_t matter_controller_interrogate_node(uint64_t node_id);
esp_err_t matter_controller_get_nodes(uint64_t *nodes, size_t max, size_t *count_out);
esp_err_t matter_controller_subscribe(void);
void      matter_controller_seed_value_cache(void);
esp_err_t matter_factory_reset(void);

#ifdef __cplusplus
}
#endif
