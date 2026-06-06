#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

// Per-node power logger. An independent periodic task pulls the current
// ElectricalPowerMeasurement/ActivePower reading from the ValueCache for every
// topology node wired to the consumer unit (appliances + solar, but NOT the
// grid — that stays in power_logger), and persists a per-minute average per
// node. Files are keyed by the stable topology graph node id so recorded data
// survives Matter device replacement.
//
// On-disk format mirrors power_logger (power_record_t), so the two streams are
// interchangeable for downstream tooling:
//   minute file : /littlefs/node-<graphId>-YYYY-MM-DD
//   hourly file : /littlefs/nodeh-<graphId>-YYYY-MM-DD

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t  node_power_logger_init(void);
char      *node_power_logger_day_json(const char *node_id, const char *date_str);    // caller must free
esp_err_t  node_power_logger_rollup_hourly(const char *date_str);                    // rolls up every node file for the date
char      *node_power_logger_hourly_json(const char *node_id, const char *date_str); // caller must free

#ifdef __cplusplus
}
#endif
