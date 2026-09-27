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
// Queue a subscription to one node now (e.g. a newly assigned tariff source).
esp_err_t matter_controller_subscribe_node(uint64_t node_id);
void      matter_controller_seed_value_cache(void);
esp_err_t matter_factory_reset(void);

#ifdef __cplusplus
}

#include <app/ConcreteAttributePath.h>
#include <app/MessageDef/StatusIB.h>
#include <lib/core/TLVReader.h>

// Attribute report sink for every subscription (see managers/subscription_manager.cpp): caches
// power/battery readings and forwards Commodity Tariff reports to tariff.cpp.
void matter_controller_attribute_data_cb(uint64_t node_id, const chip::app::ConcreteDataAttributePath &path,
                                         chip::TLV::TLVReader *data, const chip::app::StatusIB &status);
#endif
