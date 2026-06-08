#include "surplus_forecast.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "cJSON.h"

#include "power_logger.h"
#include "surplus_model.h"

static const char *TAG = "surplus_forecast";

#define LFS_BASE      "/littlefs"
#define HOURS_PER_DAY 24

// Load an hourly forecast file (prefix-date) into per-hour arrays indexed by local
// hour. Returns true if the file existed and was read.
static bool load_hourly(const char *prefix, const char *date,
                        int32_t mw[HOURS_PER_DAY], uint32_t ts[HOURS_PER_DAY])
{
    char path[64];
    snprintf(path, sizeof(path), "%s/%s-%s", LFS_BASE, prefix, date);

    FILE *f = fopen(path, "rb");
    if (!f)
        return false;

    power_record_t rec;
    while (fread(&rec, sizeof(rec), 1, f) == 1) {
        struct tm t;
        time_t s = (time_t)rec.unix_minute;
        localtime_r(&s, &t);
        int h = t.tm_hour;
        mw[h] = rec.power_mw;
        if (ts)
            ts[h] = rec.unix_minute;
    }
    fclose(f);
    return true;
}

// Cold-start fallback: surplus = solar forecast - consumption forecast, the
// approach used before the regression model had enough history to trust. Fills
// out_mw[]/out_ts[]. Returns ESP_ERR_NOT_FOUND if either input forecast is
// missing.
static esp_err_t surplus_from_subtraction(const char *date_str,
                                          int32_t out_mw[HOURS_PER_DAY],
                                          uint32_t out_ts[HOURS_PER_DAY])
{
    int32_t  solar_mw[HOURS_PER_DAY] = {0};
    uint32_t solar_ts[HOURS_PER_DAY] = {0};
    int32_t  con_mw[HOURS_PER_DAY]   = {0};

    if (!load_hourly("solar-forecast", date_str, solar_mw, solar_ts)) {
        ESP_LOGW(TAG, "No solar forecast for %s", date_str);
        return ESP_ERR_NOT_FOUND;
    }
    if (!load_hourly("consumption-forecast", date_str, con_mw, NULL)) {
        ESP_LOGW(TAG, "No consumption forecast for %s", date_str);
        return ESP_ERR_NOT_FOUND;
    }

    // Hour-start timestamps for hours the solar file didn't cover (e.g. night).
    struct tm midnight_tm = {0};
    if (!strptime(date_str, "%Y-%m-%d", &midnight_tm)) {
        ESP_LOGE(TAG, "Bad date: %s", date_str);
        return ESP_ERR_INVALID_ARG;
    }
    midnight_tm.tm_hour = 0;
    midnight_tm.tm_min  = 0;
    midnight_tm.tm_sec  = 0;
    time_t midnight = mktime(&midnight_tm);

    for (int h = 0; h < HOURS_PER_DAY; h++) {
        out_mw[h] = solar_mw[h] - con_mw[h];
        out_ts[h] = solar_ts[h] ? solar_ts[h] : (uint32_t)(midnight + h * 3600);
    }
    return ESP_OK;
}

esp_err_t surplus_forecast_compute(const char *date_str)
{
    int32_t  out_mw[HOURS_PER_DAY] = {0};
    uint32_t out_ts[HOURS_PER_DAY] = {0};
    const char *method = "regression";

    // Predict surplus directly from the solar forecast; fall back to
    // solar - consumption until the model has enough history to be trusted.
    if (!surplus_model_predict(date_str, out_mw, out_ts)) {
        method = "fallback (solar - consumption)";
        esp_err_t err = surplus_from_subtraction(date_str, out_mw, out_ts);
        if (err != ESP_OK)
            return err;
    }

    char path[64];
    snprintf(path, sizeof(path), "%s/surplus-%s", LFS_BASE, date_str);

    FILE *out = fopen(path, "wb");
    if (!out) {
        ESP_LOGE(TAG, "Cannot create %s", path);
        return ESP_FAIL;
    }

    for (int h = 0; h < HOURS_PER_DAY; h++) {
        power_record_t rec = {
            .unix_minute = out_ts[h],
            .power_mw    = out_mw[h],
        };
        fwrite(&rec, sizeof(rec), 1, out);
    }
    fclose(out);

    ESP_LOGI(TAG, "Surplus forecast computed for %s [%s]", date_str, method);
    return ESP_OK;
}

char *surplus_forecast_json(const char *date_str)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/surplus-%s", LFS_BASE, date_str);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "date", date_str);

    // Surface the regression's maturity so the UI can flag "still learning"
    // while it builds up history (it is used from day one, just not yet mature).
    uint16_t usable_days = 0;
    bool     mature      = false;
    surplus_model_status(&usable_days, &mature);
    cJSON_AddNumberToObject(root, "usable_days", usable_days);
    cJSON_AddNumberToObject(root, "mature_days", SURPLUS_MODEL_MATURE_DAYS);
    cJSON_AddBoolToObject(root, "learning", !mature);

    cJSON *slots = cJSON_AddArrayToObject(root, "slots");

    FILE *f = fopen(path, "rb");
    if (f) {
        power_record_t rec;
        while (fread(&rec, sizeof(rec), 1, f) == 1) {
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(obj, "hour_ts",   (double)rec.unix_minute);
            cJSON_AddNumberToObject(obj, "surplus_w", rec.power_mw / 1000.0);
            cJSON_AddItemToArray(slots, obj);
        }
        fclose(f);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json; // caller must free
}
