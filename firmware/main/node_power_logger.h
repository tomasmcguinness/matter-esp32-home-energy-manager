#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
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
// role is "grid" | "solar" | "load" | "battery". A stream with no file for a day
// is omitted from that day's kwh object. kWh keeps the raw Matter sign (+ = into
// the device), so a generating inverter's kWh is negative. Days with a resolved tariff also carry
// "cost":{"<id>":n,"__unmonitored":n} in major currency units, and the root
// then carries "currency" (ISO 4217 numeric). Days with grid data also carry
// "solar_kwh" and "grid_kwh" ({"<loadId>":n,"__unmonitored":n}): each load's
// energy split by source, with battery discharge attributed by where the stored
// energy came from. Caller must free.
char      *node_power_logger_daily_energy_json(int days);

// Where the energy currently held in the battery came from, per the ledger that
// drives the solar/grid split: {"known":true,"solar_pct":n,"grid_pct":n}. The
// percentages sum to 100 and cover the usable charge above the 10% reserve.
// {"known":false} when the origin is not tracked (no battery, no grid data today,
// or the battery is down to its reserve). Caller must free.
char      *node_power_logger_battery_source_json(void);

// Hour-by-hour trace of that ledger for one day (today or earlier): the starting
// ledger, then its state at the end of each local hour, with `held` in the unit
// named by "held_unit". NULL for a malformed or future date. Caller must free.
char      *node_power_logger_battery_mix_trace_json(const char *date_str);

// One stream's power for a local day as an array indexed from midnight:
// {"step_minutes":1|60,"power_w":[n|null,...]}, null where nothing was recorded,
// truncated after the last reading. Hourly values are averages of the minute
// records, so today works before the nightly rollup. Raw Matter sign. NULL for
// a malformed id or date. Caller must free.
char      *node_power_logger_series_json(const char *node_id, const char *date_str, bool hourly);

// Expected usage for a local day, per appliance and for the unmonitored
// remainder, in watts per local hour (24 values from 00:00):
// {"date","method":"same-weekday"|"recent-days","days_used":N,
//  "appliances":[{"graph_id","name","power_w":[..]}],"other_w":[..]}.
// Each value is the average of that stream's hourly history on the same weekday
// over the last 4 weeks, or over the most recent days with data when there is no
// same-weekday history. The remainder is grid + inverter output less the
// appliances, floored at 0. days_used is 0 (and every value 0) with no history at
// all. NULL for a malformed date. Caller must free.
char      *node_power_logger_usage_forecast_json(const char *date_str);

// Per-stream freshness: age of the last Matter report, and how complete today's
// minute file is. Caller must free.
char      *node_power_logger_stream_health_json(void);

// Drop the cached cost and solar/grid split for every day from date_str up to
// yesterday and rebuild them in order from the minute files. Returns the number
// of days rebuilt, or -1 for a malformed date or one more than 60 days back.
int        node_power_logger_recompute_from(const char *date_str);

#ifdef __cplusplus
}
#endif
