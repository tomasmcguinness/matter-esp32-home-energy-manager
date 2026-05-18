#include "solar_forecast.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "cJSON.h"

static const char *TAG = "solar_forecast";

// Hardcoded installation parameters — update to match site
#define FORECAST_LAT "52.0"
#define FORECAST_LON "-1.0"
#define FORECAST_DEC "35"    // panel tilt in degrees (0=flat, 90=vertical)
#define FORECAST_AZ  "0"     // azimuth: 0=south, -90=east, 90=west
#define FORECAST_KWP "4.0"   // installed peak power in kWp

#define FORECAST_URL \
    "https://api.forecast.solar/estimate/" \
    FORECAST_LAT "/" FORECAST_LON "/" FORECAST_DEC "/" FORECAST_AZ "/" FORECAST_KWP

#define INITIAL_BUF_CAP 4096

typedef struct {
    char *data;
    int   len;
    int   cap;
} resp_buf_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;

    resp_buf_t *b = evt->user_data;
    if (b->len + evt->data_len >= b->cap) {
        int new_cap = b->cap * 2 + evt->data_len;
        char *tmp = realloc(b->data, new_cap + 1);
        if (!tmp) return ESP_ERR_NO_MEM;
        b->data = tmp;
        b->cap  = new_cap;
    }
    memcpy(b->data + b->len, evt->data, evt->data_len);
    b->len += evt->data_len;
    return ESP_OK;
}

esp_err_t solar_forecast_fetch_tomorrow(cJSON **out_json)
{
    resp_buf_t buf = {
        .data = malloc(INITIAL_BUF_CAP + 1),
        .len  = 0,
        .cap  = INITIAL_BUF_CAP,
    };
    if (!buf.data) return ESP_ERR_NO_MEM;

    esp_http_client_config_t cfg = {
        .url               = FORECAST_URL,
        .event_handler     = http_event_handler,
        .user_data         = &buf,
        .timeout_ms        = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        free(buf.data);
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_perform(client);
    int status    = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGE(TAG, "request failed err=0x%x status=%d", err, status);
        free(buf.data);
        return ESP_FAIL;
    }

    buf.data[buf.len] = '\0';

    // Parse the full forecast.solar response
    cJSON *raw = cJSON_Parse(buf.data);
    free(buf.data);
    if (!raw) {
        ESP_LOGE(TAG, "JSON parse failed");
        return ESP_FAIL;
    }

    // Determine tomorrow's date by normalising to today's midnight then advancing one day.
    // Using mktime to normalise handles DST transitions correctly.
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    tm_info.tm_hour = 0;
    tm_info.tm_min  = 0;
    tm_info.tm_sec  = 0;
    tm_info.tm_mday += 1;
    mktime(&tm_info);
    char tomorrow[11];
    strftime(tomorrow, sizeof(tomorrow), "%Y-%m-%d", &tm_info);

    // Extract result.watts  — keys are "YYYY-MM-DD HH:MM:SS"
    cJSON *result  = cJSON_GetObjectItemCaseSensitive(raw, "result");
    cJSON *watts   = cJSON_GetObjectItemCaseSensitive(result, "watts");
    cJSON *wh_day  = cJSON_GetObjectItemCaseSensitive(result, "watt_hours_day");

    cJSON *out = cJSON_CreateObject();
    cJSON_AddStringToObject(out, "date", tomorrow);

    cJSON *estimates = cJSON_AddArrayToObject(out, "estimates");

    if (cJSON_IsObject(watts)) {
        cJSON *entry;
        cJSON_ArrayForEach(entry, watts) {
            // Key format: "2024-01-15 09:00:00" — only include tomorrow's entries
            const char *key = entry->string;
            if (!key || strlen(key) < 16) continue;
            if (strncmp(key, tomorrow, 10) != 0) continue;

            // Extract "HH:MM" from the key
            char time_str[6];
            strncpy(time_str, key + 11, 5);
            time_str[5] = '\0';

            cJSON *point = cJSON_CreateObject();
            cJSON_AddStringToObject(point, "time", time_str);
            cJSON_AddNumberToObject(point, "watts", entry->valuedouble);
            cJSON_AddItemToArray(estimates, point);
        }
    }

    int total_wh = 0;
    if (cJSON_IsObject(wh_day)) {
        cJSON *day_entry = cJSON_GetObjectItemCaseSensitive(wh_day, tomorrow);
        if (day_entry) {
            total_wh = (int)day_entry->valuedouble;
        }
    }
    cJSON_AddNumberToObject(out, "total_wh", total_wh);

    cJSON_Delete(raw);
    *out_json = out;
    return ESP_OK;
}
