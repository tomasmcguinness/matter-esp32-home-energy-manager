#include "power_logger.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "cJSON.h"

#define TAG          "power_logger"
#define LFS_BASE     "/littlefs"

static void date_from_unix(uint32_t ts, char *buf, size_t len)
{
    time_t t = (time_t)ts;
    struct tm tm_info;
    localtime_r(&t, &tm_info);
    strftime(buf, len, "%Y-%m-%d", &tm_info);
}

void power_logger_write_grid_minute(uint32_t unix_minute, int32_t power_mw)
{
    char date[16];
    date_from_unix(unix_minute, date, sizeof(date));

    char path[64];
    snprintf(path, sizeof(path), "%s/grid-%s", LFS_BASE, date);

    FILE *f = fopen(path, "ab");
    if (!f)
    {
        ESP_LOGW(TAG, "Cannot open %s for write", path);
        return;
    }
    power_record_t rec = {unix_minute, power_mw};
    fwrite(&rec, sizeof(rec), 1, f);
    fclose(f);
    ESP_LOGD(TAG, "Wrote grid minute %u: %d mW", unix_minute, power_mw);
}

char *power_logger_day_json(const char *date_str)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/grid-%s", LFS_BASE, date_str);

    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_AddArrayToObject(root, "records");

    FILE *f = fopen(path, "rb");
    if (f)
    {
        power_record_t rec;
        while (fread(&rec, sizeof(rec), 1, f) == 1)
        {
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(obj, "minute",  (double)rec.unix_minute);
            cJSON_AddNumberToObject(obj, "power_w", rec.power_mw / 1000.0);
            cJSON_AddItemToArray(arr, obj);
        }
        fclose(f);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json; // caller must free
}

esp_err_t power_logger_rollup_hourly(const char *date_str)
{
    char src_path[64];
    snprintf(src_path, sizeof(src_path), "%s/grid-%s", LFS_BASE, date_str);

    FILE *f = fopen(src_path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "No minute data for %s, skipping rollup", date_str);
        return ESP_ERR_NOT_FOUND;
    }

    int64_t  hour_sum[24]   = {0};
    uint32_t hour_count[24] = {0};
    uint32_t hour_ts[24]    = {0};

    power_record_t rec;
    while (fread(&rec, sizeof(rec), 1, f) == 1) {
        struct tm tm;
        time_t t = (time_t)rec.unix_minute;
        localtime_r(&t, &tm);
        int h = tm.tm_hour;
        if (hour_ts[h] == 0) {
            tm.tm_min = 0;
            tm.tm_sec = 0;
            hour_ts[h] = (uint32_t)mktime(&tm);
        }
        hour_sum[h]   += rec.power_mw;
        hour_count[h] ++;
    }
    fclose(f);

    char dst_path[64];
    snprintf(dst_path, sizeof(dst_path), "%s/grid-hourly-%s", LFS_BASE, date_str);

    FILE *out = fopen(dst_path, "wb");
    if (!out) {
        ESP_LOGE(TAG, "Cannot create %s", dst_path);
        return ESP_FAIL;
    }

    for (int h = 0; h < 24; h++) {
        if (hour_count[h] == 0) continue;
        power_record_t hrec = {
            .unix_minute = hour_ts[h],
            .power_mw    = (int32_t)(hour_sum[h] / hour_count[h]),
        };
        fwrite(&hrec, sizeof(hrec), 1, out);
    }
    fclose(out);

    ESP_LOGI(TAG, "Hourly rollup complete for %s", date_str);
    return ESP_OK;
}

char *power_logger_hourly_json(const char *date_str)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/grid-hourly-%s", LFS_BASE, date_str);

    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_AddArrayToObject(root, "records");

    FILE *f = fopen(path, "rb");
    if (f) {
        power_record_t rec;
        while (fread(&rec, sizeof(rec), 1, f) == 1) {
            cJSON *obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(obj, "hour",    (double)rec.unix_minute);
            cJSON_AddNumberToObject(obj, "power_w", rec.power_mw / 1000.0);
            cJSON_AddItemToArray(arr, obj);
        }
        fclose(f);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json; // caller must free
}
