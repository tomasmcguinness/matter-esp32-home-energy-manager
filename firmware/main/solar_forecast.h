#pragma once

#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

// Fetches the solar PV generation estimate for target_date from api.forecast.solar.
// Also saves hourly-aggregated data to /littlefs/solar-forecast-YYYY-MM-DD.
// On success, *out_json is a newly allocated cJSON object — caller must cJSON_Delete it.
// Shape: { "date": "YYYY-MM-DD", "estimates": [{"time":"HH:MM","watts":N},...], "total_wh": N }
esp_err_t solar_forecast_fetch(const char *target_date, cJSON **out_json);

// Convenience wrapper around solar_forecast_fetch() for tomorrow's date.
esp_err_t solar_forecast_fetch_tomorrow(cJSON **out_json);

// Starts the recurring 2 AM job that fetches the current day's solar forecast and
// then computes the current day's consumption forecast. If no solar forecast exists
// for today yet, runs the job once immediately (boot catch-up). Call after SNTP sync.
esp_err_t solar_forecast_start_daily_job(void);

// Read back stored hourly solar forecast. Caller must free.
// Shape: {"date":"YYYY-MM-DD","slots":[{"hour_ts":N,"power_w":F},...]}
char *solar_forecast_hourly_json(const char *date_str);

#ifdef __cplusplus
}
#endif
