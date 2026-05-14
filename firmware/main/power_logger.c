#include "power_logger.h"

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

static SemaphoreHandle_t s_mutex;

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

    esp_timer_handle_t timer;
    esp_timer_create_args_t args = {
        .callback = on_flush_timer,
        .arg      = NULL,
        .name     = "power_logger",
    };
    esp_err_t err = esp_timer_create(&args, &timer);
    if (err != ESP_OK)
        return err;

    return esp_timer_start_periodic(timer, FLUSH_US);
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
