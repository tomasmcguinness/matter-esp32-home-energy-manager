#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t unix_minute; // unix timestamp of the minute's start
    int32_t  power_mw;    // average power over the minute, in milliwatts
} power_record_t;

// Grid power is sampled by node_power_logger (it polls the ValueCache like every
// other stream); this module owns the grid's on-disk file format, rollup and
// read helpers. node_power_logger calls power_logger_write_grid_minute() to
// append one averaged minute to /sdcard/grid-YYYY-MM-DD.
void       power_logger_write_grid_minute(uint32_t unix_minute, int32_t power_mw);
char      *power_logger_day_json(const char *date_str);    // caller must free
esp_err_t  power_logger_rollup_hourly(const char *date_str);
char      *power_logger_hourly_json(const char *date_str); // caller must free

#ifdef __cplusplus
}
#endif
