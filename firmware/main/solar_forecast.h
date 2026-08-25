#pragma once

#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

// Fetches the solar PV generation estimate for target_date from api.forecast.solar.
// Also saves hourly-aggregated data to /sdcard/solar-forecast-YYYY-MM-DD.
// On success, *out_json is a newly allocated cJSON object — caller must cJSON_Delete it.
// Shape: { "date": "YYYY-MM-DD", "estimates": [{"time":"HH:MM","watts":N},...], "total_wh": N }
esp_err_t solar_forecast_fetch(const char *target_date, cJSON **out_json);

// Convenience wrapper around solar_forecast_fetch() for tomorrow's date.
esp_err_t solar_forecast_fetch_tomorrow(cJSON **out_json);

// Runs the nightly operation once for the current date: fetch the solar forecast,
// compute the consumption forecast, then derive and save the surplus forecast.
// Each step overwrites any existing file for that date. Returns the solar fetch
// error if that critical step fails, otherwise ESP_OK.
esp_err_t solar_forecast_run_daily_job(void);

// Starts the recurring 2 AM job that runs solar_forecast_run_daily_job(). If no
// solar forecast exists for the current day yet, runs the job once immediately
// (boot catch-up). Call after SNTP sync.
esp_err_t solar_forecast_start_daily_job(void);

// Notify the forecaster that the wall clock has just been set (SNTP synced). If the
// daily job is running and today's forecast is still missing, triggers the catch-up.
// Safe to call from a callback context and before solar_forecast_start_daily_job().
void solar_forecast_on_time_synced(void);

// Read back stored hourly solar forecast. Caller must free.
// Shape: {"date":"YYYY-MM-DD","slots":[{"hour_ts":N,"power_w":F},...]}
char *solar_forecast_hourly_json(const char *date_str);

#ifdef __cplusplus
}
#endif
