#include "node_power_logger.h"
#include "power_logger.h"        // power_record_t + grid file writer/rollup
#include "consumption_forecast.h"
#include "value_cache.h"
#include "managers/node_manager.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <dirent.h>
#include <string>
#include <vector>
#include <utility>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "cJSON.h"

#define TAG       "node_power_logger"
#define SD_BASE  "/sdcard"
#define SAMPLE_US (15ULL * 1000000ULL) // pull a reading every 15 s
#define FLUSH_US  (60ULL * 1000000ULL) // flush one averaged record per minute

// Matter ElectricalPowerMeasurement cluster / ActivePower attribute ids. Kept
// local so this module stays Matter-SDK-free, like ValueCache. These match the
// ids used in matter_controller.cpp and the web UI.
static constexpr uint32_t EPM_CLUSTER_ID         = 0x0090;
static constexpr uint32_t EPM_ACTIVE_POWER_ATTR  = 0x0008;

static const char *CONSUMER_UNIT_ID = "consumer_unit";
static const char *GRID_NODE_ID     = "grid_meter";

// One accumulator per recorded topology node. Mirrors power_logger's s_accum,
// but there is one per stream and it is filled by polling the ValueCache.
struct stream_t {
    std::string graph_id;       // stable topology graph node id (file key)
    uint64_t    node_id     = 0; // Matter node id (cache lookup)
    uint16_t    endpoint_id = 0; // Matter endpoint id (cache lookup)
    bool        is_grid     = false; // grid meter: persisted to the grid-* files
    int64_t     sum_mw      = 0;
    uint32_t    count       = 0;
    uint32_t    unix_minute = 0; // start of the minute being accumulated
};

static SemaphoreHandle_t     s_mutex;
static std::vector<stream_t> s_streams;
static esp_timer_handle_t    s_sample_timer;
static esp_timer_handle_t    s_flush_timer;
static esp_timer_handle_t    s_daily_timer;

static void schedule_midnight_rollup(void);

// Copy a node/date token into out only if it is filesystem-safe ([A-Za-z0-9_-],
// length 1..32). Blocks '/', '.' and anything that could escape /sdcard.
static bool sanitize_token(const char *tok, char *out, size_t out_len)
{
    if (!tok) return false;
    size_t n = strlen(tok);
    if (n == 0 || n > 32 || n >= out_len) return false;
    for (size_t i = 0; i < n; i++) {
        char c = tok[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
        out[i] = c;
    }
    out[n] = '\0';
    return true;
}

static void date_from_unix(uint32_t ts, char *buf, size_t len)
{
    time_t t = (time_t)ts;
    struct tm tm_info;
    localtime_r(&t, &tm_info);
    strftime(buf, len, "%Y-%m-%d", &tm_info);
}

static const char *json_str(cJSON *obj, const char *key)
{
    cJSON *j = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(j) ? j->valuestring : nullptr;
}

// A "distribution node" is a board that loads hang off: the main consumer unit
// or any sub consumer unit. A metered node wired to either is a stream; an edge
// between two distribution nodes (the CU -> sub-CU feed) is not, so the sub-CU
// board itself is never tracked. Works for arbitrarily nested sub-boards.
static bool is_distribution_node(cJSON *nodes, const char *id)
{
    if (!id) return false;
    if (strcmp(id, CONSUMER_UNIT_ID) == 0) return true;
    cJSON *n = nullptr;
    cJSON_ArrayForEach(n, nodes) {
        const char *nid = json_str(n, "id");
        if (!nid || strcmp(nid, id) != 0) continue;
        cJSON *settings = cJSON_GetObjectItemCaseSensitive(n, "settings");
        const char *t = settings ? json_str(settings, "type") : nullptr;
        return t && strcmp(t, "subConsumerUnit") == 0;
    }
    return false;
}

// Rebuild the set of recorded streams from the topology graph: every node wired
// to the consumer unit that maps to a Matter endpoint, including the grid. All
// streams are sampled identically (polled from the ValueCache); the grid is
// flagged so it persists to the grid-* files power_logger owns. Called at init
// and once per flush so topology edits are picked up without parsing the graph
// on every sample.
static void refresh_streams(void)
{
    char *raw = node_manager_get_all_json();
    if (!raw) return;
    cJSON *root = cJSON_Parse(raw);
    free(raw);
    if (!root) return;

    cJSON *nodes = cJSON_GetObjectItemCaseSensitive(root, "nodes");
    cJSON *edges = cJSON_GetObjectItemCaseSensitive(root, "edges");

    // Collect graph ids connected to the CU, remembering which is the grid.
    std::vector<std::pair<std::string, bool>> connected; // (graph id, is_grid)
    cJSON *e = nullptr;
    cJSON_ArrayForEach(e, edges) {
        const char *src = json_str(e, "source");
        const char *tgt = json_str(e, "target");
        if (!src || !tgt) continue;

        // The metered node is the non-distribution end of an edge that touches a
        // distribution node. Edges between two distribution nodes (CU -> sub-CU)
        // are the feed, not a stream, so they are skipped.
        bool src_dist = is_distribution_node(nodes, src);
        bool tgt_dist = is_distribution_node(nodes, tgt);

        std::string other;
        const char *cu_handle = nullptr;
        if (src_dist && !tgt_dist) { other = tgt; cu_handle = json_str(e, "sourceHandle"); }
        else if (tgt_dist && !src_dist) { other = src; cu_handle = json_str(e, "targetHandle"); }
        else continue;

        bool is_grid = other == GRID_NODE_ID || (cu_handle && strcmp(cu_handle, "grid") == 0);

        bool seen = false;
        for (const auto &c : connected) if (c.first == other) { seen = true; break; }
        if (!seen) connected.emplace_back(other, is_grid);
    }

    // Resolve each connected graph id to its Matter node/endpoint via settings.
    std::vector<stream_t> next;
    for (const auto &[gid, is_grid] : connected) {
        char clean[40];
        if (!sanitize_token(gid.c_str(), clean, sizeof(clean))) continue;

        cJSON *n = nullptr;
        cJSON_ArrayForEach(n, nodes) {
            const char *id = json_str(n, "id");
            if (!id || gid != id) continue;
            cJSON *settings = cJSON_GetObjectItemCaseSensitive(n, "settings");
            cJSON *nid = cJSON_GetObjectItemCaseSensitive(settings, "nodeId");
            cJSON *eid = cJSON_GetObjectItemCaseSensitive(settings, "endpointId");
            if (cJSON_IsNumber(nid) && cJSON_IsNumber(eid)) {
                stream_t s;
                s.graph_id    = clean;
                s.node_id     = (uint64_t)nid->valuedouble;
                s.endpoint_id = (uint16_t)eid->valuedouble;
                s.is_grid     = is_grid;
                next.push_back(std::move(s));
            }
            break;
        }
    }
    cJSON_Delete(root);

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_streams = std::move(next); // accumulators start fresh; flush runs first so nothing is lost
    xSemaphoreGive(s_mutex);

    ESP_LOGI(TAG, "Tracking %u node power stream(s)", (unsigned)s_streams.size());
}

static void on_sample_timer(void *arg)
{
    std::vector<ValueCacheEntry> snap = ValueCache::instance().snapshot();
    time_t now = time(NULL);
    uint32_t minute_start = (uint32_t)(now - (now % 60));

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (auto &s : s_streams) {
        for (const auto &v : snap) {
            if (!v.valid) continue;
            if (v.node_id != s.node_id || v.endpoint_id != s.endpoint_id) continue;
            if (v.cluster_id != EPM_CLUSTER_ID || v.attribute_id != EPM_ACTIVE_POWER_ATTR) continue;
            if (s.count == 0) s.unix_minute = minute_start;
            s.sum_mw += v.value;
            s.count++;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
}

static void write_record(const char *graph_id, const power_record_t *rec)
{
    char date[16];
    date_from_unix(rec->unix_minute, date, sizeof(date));

    char path[96];
    snprintf(path, sizeof(path), "%s/node-%s-%s", SD_BASE, graph_id, date);

    FILE *f = fopen(path, "ab");
    if (!f) {
        ESP_LOGW(TAG, "Cannot open %s for write", path);
        return;
    }
    fwrite(rec, sizeof(*rec), 1, f);
    fclose(f);
}

static void on_flush_timer(void *arg)
{
    // Drain the completed accumulators under the lock, write outside it.
    struct pending_t { std::string graph_id; power_record_t rec; bool is_grid; };
    std::vector<pending_t> pending;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (auto &s : s_streams) {
        if (s.count == 0) continue;
        power_record_t rec = { s.unix_minute, (int32_t)(s.sum_mw / s.count) };
        pending.push_back({ s.graph_id, rec, s.is_grid });
        s.sum_mw = 0;
        s.count = 0;
        s.unix_minute = 0;
    }
    xSemaphoreGive(s_mutex);

    for (const auto &p : pending) {
        if (p.is_grid)
            power_logger_write_grid_minute(p.rec.unix_minute, p.rec.power_mw);
        else
            write_record(p.graph_id.c_str(), &p.rec);
    }

    // Pick up topology edits for the next minute.
    refresh_streams();
}

static void on_midnight_timer(void *arg)
{
    time_t now = time(NULL);
    struct tm tm_info;
    localtime_r(&now, &tm_info);

    struct tm yesterday_tm = tm_info;
    yesterday_tm.tm_mday -= 1;
    mktime(&yesterday_tm);
    char yesterday[11];
    strftime(yesterday, sizeof(yesterday), "%Y-%m-%d", &yesterday_tm);

    struct tm tomorrow_tm = tm_info;
    tomorrow_tm.tm_mday += 1;
    mktime(&tomorrow_tm);
    char tomorrow[11];
    strftime(tomorrow, sizeof(tomorrow), "%Y-%m-%d", &tomorrow_tm);

    // Roll up yesterday's per-minute data to hourly, for both the per-node
    // streams and the grid, then build tomorrow's consumption forecast from the
    // freshly-rolled grid-hourly history.
    node_power_logger_rollup_hourly(yesterday);
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
    ESP_LOGI(TAG, "Daily node rollup scheduled in %llu s", (unsigned long long)(next_midnight - now));
}

esp_err_t node_power_logger_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    if (!s_mutex)
        return ESP_ERR_NO_MEM;

    refresh_streams();

    esp_timer_create_args_t sample_args = {
        .callback = on_sample_timer,
        .arg      = NULL,
        .name     = "node_pwr_sample",
    };
    esp_err_t err = esp_timer_create(&sample_args, &s_sample_timer);
    if (err != ESP_OK) return err;
    err = esp_timer_start_periodic(s_sample_timer, SAMPLE_US);
    if (err != ESP_OK) return err;

    esp_timer_create_args_t flush_args = {
        .callback = on_flush_timer,
        .arg      = NULL,
        .name     = "node_pwr_flush",
    };
    err = esp_timer_create(&flush_args, &s_flush_timer);
    if (err != ESP_OK) return err;
    err = esp_timer_start_periodic(s_flush_timer, FLUSH_US);
    if (err != ESP_OK) return err;

    esp_timer_create_args_t daily_args = {
        .callback = on_midnight_timer,
        .arg      = NULL,
        .name     = "node_pwr_daily",
    };
    err = esp_timer_create(&daily_args, &s_daily_timer);
    if (err != ESP_OK) return err;
    schedule_midnight_rollup();

    return ESP_OK;
}

char *node_power_logger_day_json(const char *node_id, const char *date_str)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_AddArrayToObject(root, "records");

    char node[40], date[16];
    if (sanitize_token(node_id, node, sizeof(node)) &&
        sanitize_token(date_str, date, sizeof(date)))
    {
        char path[96];
        snprintf(path, sizeof(path), "%s/node-%s-%s", SD_BASE, node, date);

        FILE *f = fopen(path, "rb");
        if (f) {
            power_record_t rec;
            while (fread(&rec, sizeof(rec), 1, f) == 1) {
                cJSON *obj = cJSON_CreateObject();
                cJSON_AddNumberToObject(obj, "minute",  (double)rec.unix_minute);
                cJSON_AddNumberToObject(obj, "power_w", rec.power_mw / 1000.0);
                cJSON_AddItemToArray(arr, obj);
            }
            fclose(f);
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json; // caller must free
}

static void rollup_one(const char *graph_id, const char *date)
{
    char src_path[96];
    snprintf(src_path, sizeof(src_path), "%s/node-%s-%s", SD_BASE, graph_id, date);

    FILE *f = fopen(src_path, "rb");
    if (!f) return;

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

    char dst_path[96];
    snprintf(dst_path, sizeof(dst_path), "%s/nodeh-%s-%s", SD_BASE, graph_id, date);

    FILE *out = fopen(dst_path, "wb");
    if (!out) {
        ESP_LOGE(TAG, "Cannot create %s", dst_path);
        return;
    }

    for (int h = 0; h < 24; h++) {
        if (hour_count[h] == 0) continue;
        power_record_t hrec = {
            hour_ts[h],
            (int32_t)(hour_sum[h] / hour_count[h]),
        };
        fwrite(&hrec, sizeof(hrec), 1, out);
    }
    fclose(out);
}

esp_err_t node_power_logger_rollup_hourly(const char *date_str)
{
    char date[16];
    if (!sanitize_token(date_str, date, sizeof(date)))
        return ESP_ERR_INVALID_ARG;

    DIR *dir = opendir(SD_BASE);
    if (!dir)
        return ESP_FAIL;

    // Minute files are "node-<id>-<date>"; hourly files are "nodeh-<id>-<date>"
    // and are skipped because they do not begin with the "node-" prefix.
    char suffix[20];
    snprintf(suffix, sizeof(suffix), "-%s", date);
    size_t suffix_len = strlen(suffix);

    // Collect matching graph ids first, then roll up, so we are not reading the
    // directory and opening files in it at the same time.
    std::vector<std::string> ids;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        if (strncmp(name, "node-", 5) != 0) continue;
        size_t name_len = strlen(name);
        if (name_len <= 5 + suffix_len) continue;
        if (strcmp(name + name_len - suffix_len, suffix) != 0) continue;

        size_t id_len = name_len - 5 - suffix_len;
        char graph_id[40];
        if (id_len == 0 || id_len >= sizeof(graph_id)) continue;
        memcpy(graph_id, name + 5, id_len);
        graph_id[id_len] = '\0';
        ids.emplace_back(graph_id);
    }
    closedir(dir);

    for (const auto &id : ids)
        rollup_one(id.c_str(), date);

    ESP_LOGI(TAG, "Hourly node rollup complete for %s (%u node(s))", date, (unsigned)ids.size());
    return ESP_OK;
}

char *node_power_logger_hourly_json(const char *node_id, const char *date_str)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_AddArrayToObject(root, "records");

    char node[40], date[16];
    if (sanitize_token(node_id, node, sizeof(node)) &&
        sanitize_token(date_str, date, sizeof(date)))
    {
        char path[96];
        snprintf(path, sizeof(path), "%s/nodeh-%s-%s", SD_BASE, node, date);

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
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json; // caller must free
}
