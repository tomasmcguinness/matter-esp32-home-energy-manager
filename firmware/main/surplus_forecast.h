#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Compute the hourly surplus (solar - consumption) for date_str from the stored
// solar-forecast-YYYY-MM-DD and consumption-forecast-YYYY-MM-DD hourly files,
// writing /sdcard/surplus-YYYY-MM-DD (24 power_record_t). Overwrites any existing
// file. Returns ESP_ERR_NOT_FOUND if either input forecast is missing.
esp_err_t surplus_forecast_compute(const char *date_str);

// Read back the stored surplus forecast as JSON. Caller must free.
// Shape: {"date":"YYYY-MM-DD","slots":[{"hour_ts":N,"surplus_w":F},...]}
char *surplus_forecast_json(const char *date_str);

#ifdef __cplusplus
}
#endif
