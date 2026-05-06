#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t matter_controller_start(void);
//esp_err_t matter_controller_get_fabric_info(uint64_t *fabric_id_out, uint8_t *ipk_out, size_t ipk_buf_len);
uint64_t  matter_controller_allocate_node_id(void);
// esp_err_t matter_controller_sign_noc(const uint8_t *csr_der, size_t csr_der_len,
//                                      uint64_t node_id,
//                                      uint8_t *noc_out, size_t *noc_len,
//                                      uint8_t *rcac_out, size_t *rcac_len);
esp_err_t matter_controller_commission_on_network(const char *onboarding_payload);
esp_err_t matter_controller_remove_node(uint64_t node_id);
esp_err_t matter_controller_interrogate_node(uint64_t node_id);
esp_err_t matter_controller_get_nodes(uint64_t *nodes, size_t max, size_t *count_out);
esp_err_t matter_factory_reset(void);

#ifdef __cplusplus
}
#endif
