#pragma once

#include "esp_err.h"
#include "cJSON.h"

// Fetches tomorrow's solar PV generation estimate from api.forecast.solar.
// Also saves hourly-aggregated data to /littlefs/solar-forecast-YYYY-MM-DD.
// On success, *out_json is a newly allocated cJSON object — caller must cJSON_Delete it.
// Shape: { "date": "YYYY-MM-DD", "estimates": [{"time":"HH:MM","watts":N},...], "total_wh": N }
esp_err_t solar_forecast_fetch_tomorrow(cJSON **out_json);

// Read back stored hourly solar forecast. Caller must free.
// Shape: {"date":"YYYY-MM-DD","slots":[{"hour_ts":N,"power_w":F},...]}
char *solar_forecast_hourly_json(const char *date_str);
