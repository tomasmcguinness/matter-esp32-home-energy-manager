#include "surplus_model.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include "esp_log.h"
#include "cJSON.h"

#include "power_logger.h"

static const char *TAG = "surplus_model";

#define SD_BASE        "/sdcard"
#define MODEL_PATH      SD_BASE "/surplus-model"

#define SURPLUS_MODEL_MAGIC   0x53524d31u   // "SRM1"
#define SURPLUS_MODEL_VERSION 1

#define HOURS           SURPLUS_MODEL_HOURS
#define DOWS            SURPLUS_MODEL_DOWS

#define MIN_SLOPE_DAYS  SURPLUS_MODEL_MIN_DAYS  // days at an hour needed to fit a slope
#define VAR_EPS_MW2     1e8   // ~(10 W)^2 in mW^2: below this, treat solar as constant

// Read an hourly file (prefix-date) into per-hour arrays indexed by local hour.
// present[h] marks which hours the file actually contained (rollups skip empty
// hours; the solar forecast omits night hours). Returns true if the file existed.
static bool load_hourly_file(const char *prefix, const char *date,
                             int32_t mw[HOURS], uint32_t ts[HOURS], bool present[HOURS])
{
    char path[64];
    snprintf(path, sizeof(path), "%s/%s-%s", SD_BASE, prefix, date);

    FILE *f = fopen(path, "rb");
    if (!f)
        return false;

    power_record_t rec;
    while (fread(&rec, sizeof(rec), 1, f) == 1) {
        struct tm t;
        time_t s = (time_t)rec.unix_minute;
        localtime_r(&s, &t);
        int h = t.tm_hour;
        if (h < 0 || h >= HOURS)
            continue;
        mw[h] = rec.power_mw;
        if (ts)
            ts[h] = rec.unix_minute;
        present[h] = true;
    }
    fclose(f);
    return true;
}

int surplus_model_train(int window_days)
{
    // Per-hour OLS accumulators (over all training days).
    double sx[HOURS]  = {0}, sy[HOURS]  = {0};
    double sxx[HOURS] = {0}, sxy[HOURS] = {0};
    uint32_t n[HOURS] = {0};

    // Per-(hour, day-of-week) accumulators for the intercept.
    double   bsx[HOURS][DOWS] = {{0}}, bsy[HOURS][DOWS] = {{0}};
    uint32_t bcnt[HOURS][DOWS] = {{0}};

    int usable_days = 0;

    time_t now = time(NULL);
    for (int d = 1; d <= window_days; d++) {
        struct tm day_tm;
        localtime_r(&now, &day_tm);
        day_tm.tm_mday -= d;
        day_tm.tm_hour = 12;   // midday avoids DST edge wobble when normalising
        day_tm.tm_min  = 0;
        day_tm.tm_sec  = 0;
        mktime(&day_tm);       // normalise, fill tm_wday
        int dow = day_tm.tm_wday;

        char date[11];
        strftime(date, sizeof(date), "%Y-%m-%d", &day_tm);

        int32_t  solar_mw[HOURS] = {0}, grid_mw[HOURS] = {0};
        uint32_t junk_ts[HOURS]  = {0};
        bool     solar_p[HOURS]  = {false}, grid_p[HOURS] = {false};

        // A day only teaches us the forecast->surplus relationship if it has
        // BOTH the issued solar forecast and the grid actuals (the label). Days
        // missing either are skipped entirely — otherwise daytime hours from a
        // forecast-less day would inject misleading x=0 pairs and bias the slope.
        if (!load_hourly_file("grid-hourly", date, grid_mw, junk_ts, grid_p))
            continue;
        if (!load_hourly_file("solar-forecast", date, solar_mw, junk_ts, solar_p))
            continue;
        // Within a present forecast file, a missing hour legitimately means no
        // generation was forecast (night), i.e. x = 0.

        bool contributed = false;
        for (int h = 0; h < HOURS; h++) {
            if (!grid_p[h])
                continue;
            double x = (double)solar_mw[h];        // 0 if no forecast record (night)
            double y = -(double)grid_mw[h];        // surplus = -net grid (export +ve)

            sx[h]  += x;  sy[h]  += y;
            sxx[h] += x * x;  sxy[h] += x * y;
            n[h]++;

            bsx[h][dow] += x;  bsy[h][dow] += y;  bcnt[h][dow]++;
            contributed = true;
        }
        if (contributed)
            usable_days++;
    }

    surplus_model_t m;
    memset(&m, 0, sizeof(m));
    m.magic        = SURPLUS_MODEL_MAGIC;
    m.version      = SURPLUS_MODEL_VERSION;
    m.usable_days  = (uint16_t)usable_days;
    m.trained_unix = (uint32_t)now;

    for (int h = 0; h < HOURS; h++) {
        double slope = 0.0;
        if (n[h] >= MIN_SLOPE_DAYS) {
            double mean_x = sx[h] / n[h];
            double var_x  = sxx[h] / n[h] - mean_x * mean_x;
            if (var_x > VAR_EPS_MW2) {
                double denom = (double)n[h] * sxx[h] - sx[h] * sx[h];
                slope = ((double)n[h] * sxy[h] - sx[h] * sy[h]) / denom;
            }
        }
        m.a[h] = (float)slope;

        // Hour-global intercept, used when a (hour,dow) bucket has no samples.
        double c = (n[h] > 0) ? (sy[h] - slope * sx[h]) / n[h] : 0.0;

        for (int dow = 0; dow < DOWS; dow++) {
            if (bcnt[h][dow] > 0)
                m.b[h][dow] = (float)((bsy[h][dow] - slope * bsx[h][dow]) / bcnt[h][dow]);
            else
                m.b[h][dow] = (float)c;
            m.n[h][dow] = (uint16_t)bcnt[h][dow];
        }
    }

    FILE *out = fopen(MODEL_PATH, "wb");
    if (!out) {
        ESP_LOGE(TAG, "Cannot write %s: %s", MODEL_PATH, strerror(errno));
        return usable_days;
    }
    fwrite(&m, sizeof(m), 1, out);
    fclose(out);

    ESP_LOGI(TAG, "Trained surplus model: %d usable day(s), window %d", usable_days, window_days);
    return usable_days;
}

static bool load_model(surplus_model_t *m)
{
    FILE *f = fopen(MODEL_PATH, "rb");
    if (!f)
        return false;
    bool ok = (fread(m, sizeof(*m), 1, f) == 1);
    fclose(f);
    if (!ok || m->magic != SURPLUS_MODEL_MAGIC || m->version != SURPLUS_MODEL_VERSION)
        return false;
    return true;
}

bool surplus_model_status(uint16_t *usable_days, bool *mature)
{
    surplus_model_t m;
    if (!load_model(&m)) {
        if (usable_days) *usable_days = 0;
        if (mature)      *mature = false;
        return false;
    }
    if (usable_days) *usable_days = m.usable_days;
    if (mature)      *mature = (m.usable_days >= SURPLUS_MODEL_MATURE_DAYS);
    return true;
}

bool surplus_model_predict(const char *date_str,
                           int32_t out_mw[HOURS], uint32_t out_ts[HOURS])
{
    surplus_model_t m;
    if (!load_model(&m)) {
        ESP_LOGD(TAG, "No usable model on disk");
        return false;
    }
    if (m.usable_days < SURPLUS_MODEL_MIN_DAYS) {
        ESP_LOGI(TAG, "Model has only %u day(s) (<%d) — using fallback", m.usable_days, SURPLUS_MODEL_MIN_DAYS);
        return false;
    }

    int32_t  solar_mw[HOURS] = {0};
    uint32_t solar_ts[HOURS] = {0};
    bool     solar_p[HOURS]  = {false};
    if (!load_hourly_file("solar-forecast", date_str, solar_mw, solar_ts, solar_p)) {
        ESP_LOGW(TAG, "No solar forecast for %s", date_str);
        return false;
    }

    struct tm tm = {0};
    if (!strptime(date_str, "%Y-%m-%d", &tm)) {
        ESP_LOGE(TAG, "Bad date: %s", date_str);
        return false;
    }
    tm.tm_hour = 0;
    tm.tm_min  = 0;
    tm.tm_sec  = 0;
    time_t midnight = mktime(&tm);   // also fills tm_wday
    int dow = tm.tm_wday;

    for (int h = 0; h < HOURS; h++) {
        double pred = (double)m.a[h] * (double)solar_mw[h] + (double)m.b[h][dow];
        out_mw[h] = (int32_t)lround(pred);
        out_ts[h] = solar_p[h] ? solar_ts[h] : (uint32_t)(midnight + h * 3600);
    }

    ESP_LOGI(TAG, "Surplus predicted for %s via model (%u day fit)", date_str, m.usable_days);
    return true;
}

char *surplus_model_json(void)
{
    surplus_model_t m;
    cJSON *root = cJSON_CreateObject();

    if (!load_model(&m)) {
        cJSON_AddBoolToObject(root, "trained", false);
        char *json = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
        return json;
    }

    cJSON_AddBoolToObject(root, "trained", true);
    cJSON_AddNumberToObject(root, "trained_unix", (double)m.trained_unix);
    cJSON_AddNumberToObject(root, "usable_days",  m.usable_days);

    cJSON *hours = cJSON_AddArrayToObject(root, "hours");
    for (int h = 0; h < HOURS; h++) {
        cJSON *ho = cJSON_CreateObject();
        cJSON_AddNumberToObject(ho, "hour",  h);
        cJSON_AddNumberToObject(ho, "slope", m.a[h]);
        cJSON *icpt = cJSON_AddArrayToObject(ho, "intercept");
        for (int dow = 0; dow < DOWS; dow++) {
            cJSON *bo = cJSON_CreateObject();
            cJSON_AddNumberToObject(bo, "dow",   dow);
            cJSON_AddNumberToObject(bo, "value", m.b[h][dow]);
            cJSON_AddNumberToObject(bo, "n",     m.n[h][dow]);
            cJSON_AddItemToArray(icpt, bo);
        }
        cJSON_AddItemToArray(hours, ho);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json; // caller must free
}
