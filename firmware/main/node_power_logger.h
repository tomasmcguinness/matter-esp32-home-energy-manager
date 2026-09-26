#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

// Power logger for every topology node wired to the consumer unit. An
// independent periodic task pulls the current ElectricalPowerMeasurement/
// ActivePower reading from the ValueCache for each stream (appliances, solar
// AND the grid) and persists a per-minute average. Polling the cache — rather
// than logging on each Matter report — keeps a stalled or bursty subscription
// from leaving gaps in the record.
//
// Per-node files are keyed by the stable topology graph node id so recorded
// data survives Matter device replacement. The grid is flagged and persisted to
// the grid-* files power_logger owns, keeping the consumption-forecast pipeline
// and web API unchanged. On-disk format is power_record_t throughout:
//   node minute file : /sdcard/node-<graphId>-YYYY-MM-DD
//   node hourly file : /sdcard/nodeh-<graphId>-YYYY-MM-DD
//   grid minute file : /sdcard/grid-YYYY-MM-DD
//   grid hourly file : /sdcard/grid-hourly-YYYY-MM-DD

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t  node_power_logger_init(void);
char      *node_power_logger_day_json(const char *node_id, const char *date_str);    // caller must free
esp_err_t  node_power_logger_rollup_hourly(const char *date_str);                    // rolls up every node file for the date
char      *node_power_logger_hourly_json(const char *node_id, const char *date_str); // caller must free

// Per-stream daily energy (kWh) for the last `days` local dates, today included,
// oldest first: {"nodes":[{"id","role"}], "days":[{"date","kwh":{"<id>":n}}]}.
// role is "grid" | "solar" | "load". A stream with no file for a day is omitted
// from that day's kwh object. Caller must free.
char      *node_power_logger_daily_energy_json(int days);

#ifdef __cplusplus
}
#endif
