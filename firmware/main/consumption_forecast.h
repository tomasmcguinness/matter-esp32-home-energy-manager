#pragma once

#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

// Compute next-day consumption forecast using same-weekday hourly baseline.
// Looks back up to 4 prior same-weekday grid-hourly files; works from day 1
// (a single prior week is sufficient). Writes the result to
// /littlefs/consumption-forecast-{target_date} as 24 power_record_t records.
// Returns ESP_ERR_NOT_FOUND if no prior same-weekday data exists at all.
esp_err_t consumption_forecast_compute(const char *target_date);

// Read back a stored forecast as JSON. Caller must free.
// Shape: {"date":"YYYY-MM-DD","slots":[{"hour_ts":N,"power_w":F},...]}
char *consumption_forecast_json(const char *date_str);

#ifdef __cplusplus
}
#endif
