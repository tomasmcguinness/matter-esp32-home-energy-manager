#include "consumption_forecast.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "cJSON.h"

#include "power_logger.h"

static const char *TAG = "consumption_forecast";

#define LFS_BASE       "/littlefs"
#define LOOKBACK_WEEKS 4
#define HOURS_PER_DAY  24

esp_err_t consumption_forecast_compute(const char *target_date)
{
    // Parse target_date into a struct tm so we can step back by weeks.
    struct tm target_tm = {0};
    if (!strptime(target_date, "%Y-%m-%d", &target_tm)) {
        ESP_LOGE(TAG, "Bad date: %s", target_date);
        return ESP_ERR_INVALID_ARG;
    }
    mktime(&target_tm); // normalise and fill tm_wday

    int64_t  sum_mw[HOURS_PER_DAY]   = {0};
    uint32_t count[HOURS_PER_DAY]    = {0};
    int      days_used               = 0;

    for (int w = 1; w <= LOOKBACK_WEEKS; w++) {
        struct tm prior_tm = target_tm;
        prior_tm.tm_mday -= w * 7;
        mktime(&prior_tm);

        char prior_date[11];
        strftime(prior_date, sizeof(prior_date), "%Y-%m-%d", &prior_tm);

        char path[64];
        snprintf(path, sizeof(path), "%s/grid-hourly-%s", LFS_BASE, prior_date);

        FILE *f = fopen(path, "rb");
        if (!f) {
            ESP_LOGD(TAG, "No hourly data for %s, skipping", prior_date);
            continue;
        }

        power_record_t rec;
        while (fread(&rec, sizeof(rec), 1, f) == 1) {
            struct tm rec_tm;
            time_t t = (time_t)rec.unix_minute;
            localtime_r(&t, &rec_tm);
            int h = rec_tm.tm_hour;
            sum_mw[h] += rec.power_mw;
            count[h]++;
        }
        fclose(f);
        days_used++;
    }

    if (days_used == 0) {
        ESP_LOGW(TAG, "No prior same-weekday data for %s", target_date);
        return ESP_ERR_NOT_FOUND;
    }

    // Build hour-start timestamps for target_date.
    struct tm midnight_tm = target_tm;
    midnight_tm.tm_hour = 0;
    midnight_tm.tm_min  = 0;
    midnight_tm.tm_sec  = 0;
    time_t midnight = mktime(&midnight_tm);

    char dst_path[64];
    snprintf(dst_path, sizeof(dst_path), "%s/consumption-forecast-%s", LFS_BASE, target_date);

    FILE *out = fopen(dst_path, "wb");
    if (!out) {
        ESP_LOGE(TAG, "Cannot create %s", dst_path);
        return ESP_FAIL;
    }

    int gaps = 0;
    for (int h = 0; h < HOURS_PER_DAY; h++) {
        power_record_t rec = {
            .unix_minute = (uint32_t)(midnight + h * 3600),
            .power_mw    = (count[h] > 0) ? (int32_t)(sum_mw[h] / count[h]) : 0,
        };
        if (count[h] == 0) gaps++;
        fwrite(&rec, sizeof(rec), 1, out);
    }
    fclose(out);

    ESP_LOGI(TAG, "Forecast for %s: %d prior day(s), %d gap hour(s)", target_date, days_used, gaps);
    return ESP_OK;
}

char *consumption_forecast_json(const char *date_str)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/consumption-forecast-%s", LFS_BASE, date_str);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "date", date_str);
    cJSON *slots = cJSON_AddArrayToObject(root, "slots");

    FILE *f = fopen(path, "rb");
    if (f) {
        power_record_t rec;
        while (fread(&rec, sizeof(rec), 1, f) == 1) {
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(obj, "hour_ts", (double)rec.unix_minute);
            cJSON_AddNumberToObject(obj, "power_w",  rec.power_mw / 1000.0);
            cJSON_AddItemToArray(slots, obj);
        }
        fclose(f);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json; // caller must free
}
