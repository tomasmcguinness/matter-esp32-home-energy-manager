#include "power_logger.h"
#include "consumption_forecast.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "cJSON.h"

#define TAG          "power_logger"
#define LFS_BASE     "/littlefs"
#define FLUSH_US     (60ULL * 1000000ULL) // 60 seconds

static SemaphoreHandle_t    s_mutex;
static esp_timer_handle_t   s_daily_timer;

static void schedule_midnight_rollup(void);

static void on_midnight_timer(void *arg)
{
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);

    // Yesterday: one day before the new day we just entered.
    struct tm yesterday_tm = tm_info;
    yesterday_tm.tm_mday -= 1;
    mktime(&yesterday_tm);
    char yesterday[11];
    strftime(yesterday, sizeof(yesterday), "%Y-%m-%d", &yesterday_tm);

    // Tomorrow: one day ahead, for the consumption forecast.
    struct tm tomorrow_tm = tm_info;
    tomorrow_tm.tm_mday += 1;
    mktime(&tomorrow_tm);
    char tomorrow[11];
    strftime(tomorrow, sizeof(tomorrow), "%Y-%m-%d", &tomorrow_tm);

    power_logger_rollup_hourly(yesterday);
    consumption_forecast_compute(tomorrow);
    schedule_midnight_rollup();
}

static void schedule_midnight_rollup(void)
{
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);
    tm_info.tm_hour = 0;
    tm_info.tm_min  = 0;
    tm_info.tm_sec  = 0;
    tm_info.tm_mday += 1;
    time_t next_midnight = mktime(&tm_info);
    uint64_t delay_us = (uint64_t)(next_midnight - now) * 1000000ULL;
    esp_timer_start_once(s_daily_timer, delay_us);
    ESP_LOGI(TAG, "Daily rollup scheduled in %llu s", (unsigned long long)(next_midnight - now));
}

static struct {
    int64_t  sum_mw;
    uint32_t count;
    uint32_t unix_minute; // start of the minute being accumulated
} s_accum;

static void date_from_unix(uint32_t ts, char *buf, size_t len)
{
    time_t t = (time_t)ts;
    struct tm tm_info;
    localtime_r(&t, &tm_info);
    strftime(buf, len, "%Y-%m-%d", &tm_info);
}

static void write_record(uint32_t unix_minute, int32_t power_mw)
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
}

static void on_flush_timer(void *arg)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_accum.count == 0)
    {
        xSemaphoreGive(s_mutex);
        return;
    }
    int32_t  avg_mw      = (int32_t)(s_accum.sum_mw / s_accum.count);
    uint32_t unix_minute = s_accum.unix_minute;
    s_accum.sum_mw    = 0;
    s_accum.count     = 0;
    s_accum.unix_minute = 0;
    xSemaphoreGive(s_mutex);

    write_record(unix_minute, avg_mw);
    ESP_LOGD(TAG, "Flushed minute %u: %d mW", unix_minute, avg_mw);
}

esp_err_t power_logger_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex)
        return ESP_ERR_NO_MEM;

    esp_timer_handle_t flush_timer;
    esp_timer_create_args_t flush_args = {
        .callback = on_flush_timer,
        .arg      = NULL,
        .name     = "power_logger_flush",
    };
    esp_err_t err = esp_timer_create(&flush_args, &flush_timer);
    if (err != ESP_OK)
        return err;
    err = esp_timer_start_periodic(flush_timer, FLUSH_US);
    if (err != ESP_OK)
        return err;

    esp_timer_create_args_t daily_args = {
        .callback = on_midnight_timer,
        .arg      = NULL,
        .name     = "power_logger_daily",
    };
    err = esp_timer_create(&daily_args, &s_daily_timer);
    if (err != ESP_OK)
        return err;
    schedule_midnight_rollup();

    return ESP_OK;
}

void power_logger_sample(int32_t power_mw)
{
    time_t now = time(NULL);
    // Round down to the current minute boundary
    uint32_t minute_start = (uint32_t)(now - (now % 60));

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_accum.count == 0)
        s_accum.unix_minute = minute_start;
    s_accum.sum_mw += power_mw;
    s_accum.count++;
    xSemaphoreGive(s_mutex);
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
