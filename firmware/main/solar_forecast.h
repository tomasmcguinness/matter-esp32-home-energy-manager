#pragma once

#include "esp_err.h"
#include "cJSON.h"

// Fetches tomorrow's solar PV generation estimate from api.forecast.solar.
// On success, *out_json is a newly allocated cJSON object — caller must cJSON_Delete it.
// Shape: { "date": "YYYY-MM-DD", "estimates": [{"time":"HH:MM","watts":N},...], "total_wh": N }
esp_err_t solar_forecast_fetch_tomorrow(cJSON **out_json);
