#include "node_power_logger.h"
#include "power_logger.h"        // power_record_t + grid file writer/rollup
#include "consumption_forecast.h"
#include "value_cache.h"
#include "tariff.h"
#include "managers/node_manager.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <math.h>
#include <dirent.h>
#include <unistd.h>
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
// Power Source cluster / BatPercentRemaining (half-percent units, 0..200).
static constexpr uint32_t PS_CLUSTER_ID          = 0x002F;
static constexpr uint32_t PS_BAT_PERCENT_ATTR    = 0x000C;

static const char *CONSUMER_UNIT_ID = "consumer_unit";
static const char *GRID_NODE_ID     = "grid_meter";

// One accumulator per recorded topology node. Mirrors power_logger's s_accum,
// but there is one per stream and it is filled by polling the ValueCache.
struct stream_t {
    std::string graph_id;       // stable topology graph node id (file key)
    uint64_t    node_id     = 0; // Matter node id (cache lookup)
    uint16_t    endpoint_id = 0; // Matter endpoint id (cache lookup)
    bool        is_grid     = false; // grid meter: persisted to the grid-* files
    const char *role        = "load"; // "grid" | "solar" | "load" from the CU handle, or "battery"
    int64_t     sum_mw      = 0;
    uint32_t    count       = 0;
    uint32_t    unix_minute = 0; // start of the minute being accumulated
    int32_t     soc_half_pct = -1; // battery only: latest state of charge, -1 = none seen
};

static SemaphoreHandle_t     s_mutex;
static std::vector<stream_t> s_streams;
static esp_timer_handle_t    s_sample_timer;
static esp_timer_handle_t    s_flush_timer;
static esp_timer_handle_t    s_daily_timer;

static void schedule_midnight_rollup(void);
static void cost_day_cached(const char *date, bool allow_cache_write);
static void split_day_cached(const char *date);

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
    std::vector<std::pair<std::string, const char *>> connected; // (graph id, role)
    cJSON *e = nullptr;
    cJSON_ArrayForEach(e, edges) {
        const char *src = json_str(e, "source");
        const char *tgt = json_str(e, "target");
        if (!src || !tgt) continue;

        // The home battery hangs off the inverter's `battery` handle rather than
        // the CU. Record it too so the solar/grid split can track what it stores.
        const char *src_handle = json_str(e, "sourceHandle");
        if (src_handle && strcmp(src_handle, "battery") == 0) {
            bool seen = false;
            for (const auto &c : connected) if (c.first == tgt) { seen = true; break; }
            if (!seen) connected.emplace_back(tgt, "battery");
            continue;
        }

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

        // The tariff source hangs off the CU but meters nothing.
        if (cu_handle && strcmp(cu_handle, "tariff") == 0) continue;

        bool is_grid = other == GRID_NODE_ID || (cu_handle && strcmp(cu_handle, "grid") == 0);
        const char *role = is_grid ? "grid"
                         : (cu_handle && strncmp(cu_handle, "solar", 5) == 0) ? "solar"
                         : "load";

        bool seen = false;
        for (const auto &c : connected) if (c.first == other) { seen = true; break; }
        if (!seen) connected.emplace_back(other, role);
    }

    // Resolve each connected graph id to its Matter node/endpoint via settings.
    std::vector<stream_t> next;
    for (const auto &[gid, role] : connected) {
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
                s.role        = role;
                s.is_grid     = strcmp(role, "grid") == 0;
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
            s.sum_mw += v.value; // stored with the raw Matter sign (+ = into the device)
            s.count++;
            break;
        }
        // The battery's state of charge anchors the solar/grid ledger.
        if (strcmp(s.role, "battery") != 0) continue;
        for (const auto &v : snap) {
            if (!v.valid) continue;
            if (v.node_id != s.node_id || v.endpoint_id != s.endpoint_id) continue;
            if (v.cluster_id != PS_CLUSTER_ID || v.attribute_id != PS_BAT_PERCENT_ATTR) continue;
            if (v.value >= 0 && v.value <= 200) s.soc_half_pct = (int32_t)v.value;
            break;
        }
    }
    xSemaphoreGive(s_mutex);
}

static void write_record(const char *prefix, const char *graph_id, const power_record_t *rec)
{
    char date[16];
    date_from_unix(rec->unix_minute, date, sizeof(date));

    char path[96];
    snprintf(path, sizeof(path), "%s/%s-%s-%s", SD_BASE, prefix, graph_id, date);

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
    struct pending_t { std::string graph_id; power_record_t rec; bool is_grid; int32_t soc_half_pct; };
    std::vector<pending_t> pending;

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (auto &s : s_streams) {
        if (s.count == 0) continue;
        power_record_t rec = { s.unix_minute, (int32_t)(s.sum_mw / s.count) };
        pending.push_back({ s.graph_id, rec, s.is_grid, s.soc_half_pct });
        s.sum_mw = 0;
        s.count = 0;
        s.unix_minute = 0;
        s.soc_half_pct = -1;
    }
    xSemaphoreGive(s_mutex);

    for (const auto &p : pending) {
        if (p.is_grid)
            power_logger_write_grid_minute(p.rec.unix_minute, p.rec.power_mw);
        else
            write_record("node", p.graph_id.c_str(), &p.rec);
        if (p.soc_half_pct >= 0) {
            power_record_t soc = { p.rec.unix_minute, p.soc_half_pct };
            write_record("soc", p.graph_id.c_str(), &soc);
        }
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
    cost_day_cached(yesterday, true); // freeze yesterday's cost while the minute files are fresh
    split_day_cached(yesterday);      // ...and its solar/grid split, carrying the battery ledger on
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

// Energy in kWh for one stream-day. The hourly rollup is preferred (each record
// is a 1-hour average, so Σ mW / 1e6 = kWh); days not yet rolled up (today, or
// a missed midnight job) fall back to the minute file (Σ mW / 60 / 1e6).
// Returns false when neither file exists so callers can tell "no data" from 0.
static bool file_kwh(const char *hourly_path, const char *minute_path, double *kwh)
{
    const char *paths[2]   = { hourly_path, minute_path };
    const double divisor[2] = { 1e6, 60.0 * 1e6 };
    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(paths[i], "rb");
        if (!f) continue;
        int64_t sum_mw = 0;
        power_record_t rec;
        while (fread(&rec, sizeof(rec), 1, f) == 1)
            sum_mw += rec.power_mw;
        fclose(f);
        *kwh = (double)sum_mw / divisor[i];
        return true;
    }
    return false;
}

// Power a device supplies to the premises, from its stored reading. Readings are
// kept with the raw Matter sign (+ = into the device, - = the device supplying),
// so an inverter generating or a battery discharging reads negative. Grid and
// load readings are used as stored: + = import / consumption.
static inline int32_t supplied_mw(int32_t raw_mw) { return -raw_mw; }

struct stream_info_t { std::string graph_id; const char *role; bool is_grid; };

static std::vector<stream_info_t> snapshot_streams(void)
{
    std::vector<stream_info_t> streams;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (const auto &s : s_streams)
        streams.push_back({ s.graph_id, s.role, s.is_grid });
    xSemaphoreGive(s_mutex);
    return streams;
}

// Minutes in a local day (1380..1500 across DST changes), indexed from local midnight.
static constexpr int kMaxDayMinutes = 1500;

// Load a minute file into per-minute-of-day mW, indexed from day_start. Returns
// false if the file is absent. Minutes with no record stay at `fill`.
static bool load_minutes(const char *path, time_t day_start, std::vector<int32_t> &mw, int32_t fill = 0)
{
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    mw.assign(kMaxDayMinutes, fill);
    power_record_t rec;
    while (fread(&rec, sizeof(rec), 1, f) == 1) {
        long idx = ((long)rec.unix_minute - (long)day_start) / 60;
        if (idx >= 0 && idx < kMaxDayMinutes) mw[idx] = rec.power_mw;
    }
    fclose(f);
    return true;
}

struct day_cost_t {
    uint16_t currency = 0;
    uint8_t  decimals = 0;
    std::vector<std::pair<std::string, double>> cost; // graph id (or UNMONITORED_KEY) -> money units
};

static const char *UNMONITORED_KEY = "__unmonitored";

// Cost one day from its minute files and resolved tariff. The bill only charges
// grid import, so each minute costs max(grid, 0) kWh x that slot's price; that
// cost is then shared across the appliances and the unmonitored remainder in
// proportion to their share of the minute's consumption (grid + solar). The
// per-device costs therefore add up to the import cost, and solar-covered
// consumption is free. Returns false when there is no tariff or grid data.
static bool compute_day_cost(const char *date, const std::vector<stream_info_t> &streams, day_cost_t &out)
{
    tariff_day_t tariff;
    if (!tariff_load_day(date, &tariff)) return false;

    struct tm tm_day = {};
    if (sscanf(date, "%d-%d-%d", &tm_day.tm_year, &tm_day.tm_mon, &tm_day.tm_mday) != 3) return false;
    tm_day.tm_year -= 1900;
    tm_day.tm_mon  -= 1;
    tm_day.tm_isdst = -1;
    time_t day_start = mktime(&tm_day);

    char path[96];
    std::vector<int32_t> grid, solar(kMaxDayMinutes, 0), tmp;
    snprintf(path, sizeof(path), "%s/grid-%s", SD_BASE, date);
    if (!load_minutes(path, day_start, grid)) return false;

    std::vector<std::pair<std::string, std::vector<int32_t>>> loads;
    for (const auto &s : streams) {
        if (s.is_grid || strcmp(s.role, "battery") == 0) continue; // the battery is not a load
        snprintf(path, sizeof(path), "%s/node-%s-%s", SD_BASE, s.graph_id.c_str(), date);
        if (!load_minutes(path, day_start, tmp)) tmp.assign(kMaxDayMinutes, 0);
        if (strcmp(s.role, "solar") == 0) {
            for (int i = 0; i < kMaxDayMinutes; i++) solar[i] += supplied_mw(tmp[i]);
        } else {
            loads.emplace_back(s.graph_id, tmp);
        }
    }

    std::vector<double> load_cost(loads.size(), 0.0);
    double unmonitored_cost = 0.0;

    for (int i = 0; i < kMaxDayMinutes; i++) {
        if (grid[i] <= 0) continue; // exporting or idle: nothing bought this minute

        time_t t = day_start + (time_t)i * 60;
        struct tm lt;
        localtime_r(&t, &lt);
        int slot = (lt.tm_hour * 60 + lt.tm_min) / TARIFF_SLOT_MINUTES;
        if (slot < 0 || slot >= TARIFF_SLOTS || tariff.price[slot] == TARIFF_NO_PRICE) continue;

        double minute_cost = (grid[i] / 60.0 / 1e6) * (double)tariff.price[slot];

        double total = (double)grid[i] + (double)solar[i];
        double loads_sum = 0.0;
        for (const auto &l : loads) loads_sum += l.second[i] > 0 ? l.second[i] : 0;
        if (loads_sum > total) total = loads_sum; // metering disagreement: never negative remainder
        if (total <= 0) {
            unmonitored_cost += minute_cost;
            continue;
        }
        for (size_t k = 0; k < loads.size(); k++) {
            int32_t v = loads[k].second[i];
            if (v > 0) load_cost[k] += minute_cost * (v / total);
        }
        unmonitored_cost += minute_cost * ((total - loads_sum) / total);
    }

    out.currency = tariff.currency;
    out.decimals = tariff.decimals;
    out.cost.clear();
    for (size_t k = 0; k < loads.size(); k++) out.cost.emplace_back(loads[k].first, load_cost[k]);
    out.cost.emplace_back(UNMONITORED_KEY, unmonitored_cost);
    return true;
}

static void cost_path(const char *date, char *buf, size_t len)
{
    snprintf(buf, len, "%s/cost-%s", SD_BASE, date);
}

static bool read_cost_cache(const char *date, day_cost_t &out)
{
    char path[64];
    cost_path(date, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 8192) { fclose(f); return false; }
    std::string buf((size_t)len, '\0');
    size_t got = fread(&buf[0], 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len) return false;

    cJSON *root = cJSON_Parse(buf.c_str());
    if (!root) return false;
    cJSON *cur  = cJSON_GetObjectItemCaseSensitive(root, "currency");
    cJSON *dec  = cJSON_GetObjectItemCaseSensitive(root, "decimals");
    cJSON *cost = cJSON_GetObjectItemCaseSensitive(root, "cost");
    bool ok = cJSON_IsNumber(cur) && cJSON_IsNumber(dec) && cJSON_IsObject(cost);
    if (ok) {
        out.currency = (uint16_t)cur->valuedouble;
        out.decimals = (uint8_t)dec->valuedouble;
        out.cost.clear();
        cJSON *c = nullptr;
        cJSON_ArrayForEach(c, cost)
            if (cJSON_IsNumber(c)) out.cost.emplace_back(c->string, c->valuedouble);
    }
    cJSON_Delete(root);
    return ok;
}

static void write_cost_cache(const char *date, const day_cost_t &dc)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "currency", dc.currency);
    cJSON_AddNumberToObject(root, "decimals", dc.decimals);
    cJSON *cost = cJSON_AddObjectToObject(root, "cost");
    for (const auto &c : dc.cost) cJSON_AddNumberToObject(cost, c.first.c_str(), c.second);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return;

    char path[64];
    cost_path(date, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (f) {
        fputs(json, f);
        fclose(f);
    }
    free(json);
}

// A finished day's minute files and tariff never change, so its cost is
// computed once and cached. Today is always computed live and never cached.
static bool day_cost(const char *date, bool is_today, const std::vector<stream_info_t> &streams,
                     day_cost_t &out, bool allow_cache_write)
{
    if (!is_today && read_cost_cache(date, out)) return true;
    if (!compute_day_cost(date, streams, out)) return false;
    if (!is_today && allow_cache_write) write_cost_cache(date, out);
    return true;
}

static void cost_day_cached(const char *date, bool allow_cache_write)
{
    day_cost_t dc;
    day_cost(date, false, snapshot_streams(), dc, allow_cache_write);
}

// ---- Solar vs grid energy split ---------------------------------------------
// Each minute, house consumption (grid + inverter AC output) is supplied by grid
// import, PV directly, and battery discharge. Every load (and the unmonitored
// remainder) takes the same mix, in proportion to its draw, like the cost split
// above. Battery discharge is solar or grid according to where the energy it
// holds came from, tracked by a ledger carried from day to day. Files hold the
// raw Matter sign, so the inverter and battery are converted with supplied_mw().
// The inverter AC output is assumed to net battery flow (DC-coupled hybrid
// inverter), so a negative output while charging is power pulled from the grid.

// Energy held in the battery and the fraction of it that came from solar.
// Unknown origin (cold start, or drained) counts as grid, so the split never
// overstates self-consumption. On days with a state-of-charge log the energy is
// the usable charge above the reserve, in half-percent units (`soc`), so it
// cannot drift from what the battery really holds; on older days it is Wh
// integrated from the battery's power.
struct battery_ledger_t {
    double e_wh  = 0.0;
    double f     = 0.0;
    bool   known = false;
    bool   soc   = false; // e_wh is usable state of charge, not Wh
};

struct day_split_t {
    // graph id (or UNMONITORED_KEY) -> (solar Wh, grid Wh)
    std::vector<std::pair<std::string, std::pair<double, double>>> wh;
    battery_ledger_t end;
};

static constexpr double kBatteryRoundTrip = 0.9;
// The inverter never discharges below this, so charge under it is not counted.
static constexpr int32_t kBatteryReserveHalfPct = 20; // 10 %

// Local midnight for a YYYY-MM-DD date.
static bool local_day_start(const char *date, time_t *out)
{
    struct tm tm_day = {};
    if (sscanf(date, "%d-%d-%d", &tm_day.tm_year, &tm_day.tm_mon, &tm_day.tm_mday) != 3) return false;
    tm_day.tm_year -= 1900;
    tm_day.tm_mon  -= 1;
    tm_day.tm_isdst = -1;
    *out = mktime(&tm_day);
    return true;
}

static bool compute_day_split(const char *date, const std::vector<stream_info_t> &streams,
                              const battery_ledger_t &start, day_split_t &out,
                              std::vector<battery_ledger_t> *hourly = nullptr)
{
    time_t day_start;
    if (!local_day_start(date, &day_start)) return false;

    char path[96];
    std::vector<int32_t> grid, inv(kMaxDayMinutes, 0), bat(kMaxDayMinutes, 0), soc, tmp;
    bool have_soc = false;
    snprintf(path, sizeof(path), "%s/grid-%s", SD_BASE, date);
    if (!load_minutes(path, day_start, grid)) return false;

    std::vector<std::pair<std::string, std::vector<int32_t>>> loads;
    for (const auto &s : streams) {
        if (s.is_grid) continue;
        snprintf(path, sizeof(path), "%s/node-%s-%s", SD_BASE, s.graph_id.c_str(), date);
        if (!load_minutes(path, day_start, tmp)) tmp.assign(kMaxDayMinutes, 0);
        if (strcmp(s.role, "solar") == 0) {
            for (int i = 0; i < kMaxDayMinutes; i++) inv[i] += supplied_mw(tmp[i]);
        } else if (strcmp(s.role, "battery") == 0) {
            for (int i = 0; i < kMaxDayMinutes; i++) bat[i] += supplied_mw(tmp[i]);
            snprintf(path, sizeof(path), "%s/soc-%s-%s", SD_BASE, s.graph_id.c_str(), date);
            if (!have_soc) have_soc = load_minutes(path, day_start, soc, -1);
        } else {
            loads.emplace_back(s.graph_id, tmp);
        }
    }

    std::vector<double> load_solar(loads.size(), 0.0), load_grid(loads.size(), 0.0);
    double un_solar = 0.0, un_grid = 0.0;
    battery_ledger_t L = start;
    // A ledger in Wh is re-based onto the first state-of-charge reading, keeping
    // its mix. The reverse has no conversion, so the history is dropped.
    bool rebase = have_soc && !L.soc;
    if (!have_soc && L.soc) L = battery_ledger_t{};
    double pend_s = 0.0, pend_g = 0.0; // charge since the last state-of-charge step

    for (int i = 0; i < kMaxDayMinutes; i++) {
        double grid_w = grid[i] / 1000.0;
        double inv_w  = inv[i] / 1000.0;
        double bat_w  = bat[i] / 1000.0; // supplied: + discharging, - charging

        double loads_sum = 0.0;
        for (const auto &l : loads) loads_sum += l.second[i] > 0 ? l.second[i] / 1000.0 : 0.0;
        double total = grid_w + inv_w;
        if (loads_sum > total) total = loads_sum; // metering disagreement: never negative remainder

        if (total > 0) {
            // Grid share of the house (capped: when the inverter charges the
            // battery from the grid, import exceeds house use).
            double g = grid_w > 0 ? grid_w / total : 0.0;
            if (g > 1.0) g = 1.0;
            // Of the inverter's output, the part that came out of the battery.
            double inv_pos = inv_w > 0 ? inv_w : 0.0;
            double dis     = bat_w > 0 ? bat_w : 0.0;
            double b = inv_pos > 0 ? (dis < inv_pos ? dis : inv_pos) / inv_pos : 0.0;
            double f = L.known ? L.f : 0.0;
            double s = (1.0 - g) * ((1.0 - b) + b * f);

            for (size_t k = 0; k < loads.size(); k++) {
                int32_t v = loads[k].second[i];
                if (v <= 0) continue;
                double wh = v / 1000.0 / 60.0;
                load_solar[k] += wh * s;
                load_grid[k]  += wh * (1.0 - s);
            }
            double un_wh = (total - loads_sum) / 60.0;
            un_solar += un_wh * s;
            un_grid  += un_wh * (1.0 - s);
        }

        // Update the battery ledger with this minute's flow.
        if (have_soc) {
            if (bat_w < 0) {
                double c  = -bat_w;
                double cg = inv_w < 0 ? (-inv_w < c ? -inv_w : c) : 0.0; // pulled from the AC side
                pend_s += c - cg;
                pend_g += cg;
            }
            if (soc[i] >= 0) { // else no reading this minute
                double u    = soc[i] > kBatteryReserveHalfPct ? soc[i] - kBatteryReserveHalfPct : 0.0;
                double held = L.known ? L.e_wh : 0.0;
                if (u <= 0) {
                    L = battery_ledger_t{}; // down to the reserve: nothing usable left
                    pend_s = pend_g = 0.0;  // charge below the reserve is not counted
                } else if (rebase && L.known) {
                    L.e_wh = u;
                    pend_s = pend_g = 0.0;
                } else if (u > held) {
                    // The rise takes the mix of the charge that produced it; a rise
                    // with no charge recorded keeps the current mix (grid if unknown).
                    double pend = pend_s + pend_g;
                    double r = pend > 0 ? pend_s / pend : (L.known ? L.f : 0.0);
                    L.f     = (L.f * held + r * (u - held)) / u;
                    L.e_wh  = u;
                    L.known = true;
                    pend_s = pend_g = 0.0;
                } else if (u < held) {
                    L.e_wh = u;
                    pend_s = pend_g = 0.0;
                }
                rebase = false;
            }
        } else if (bat_w < 0) {
            double c  = -bat_w;
            double cg = inv_w < 0 ? (-inv_w < c ? -inv_w : c) : 0.0; // pulled from the AC side
            double cs = c - cg;
            double solar_held = L.known ? L.f * L.e_wh : 0.0;
            L.e_wh  = (L.known ? L.e_wh : 0.0) + c / 60.0;
            L.f     = (solar_held + cs / 60.0) / L.e_wh;
            L.known = true;
        } else if (bat_w > 0 && L.known) {
            L.e_wh -= bat_w / 60.0 / kBatteryRoundTrip;
            if (L.e_wh <= 0) L = battery_ledger_t{};
        }

        if (hourly && i % 60 == 59) {
            hourly->push_back(L);
            hourly->back().soc = have_soc;
        }
    }

    out.wh.clear();
    for (size_t k = 0; k < loads.size(); k++)
        out.wh.push_back({ loads[k].first, { load_solar[k], load_grid[k] } });
    out.wh.push_back({ UNMONITORED_KEY, { un_solar, un_grid } });
    out.end = L;
    out.end.soc = have_soc;
    return true;
}

static void split_path(const char *date, char *buf, size_t len)
{
    snprintf(buf, len, "%s/split-%s", SD_BASE, date);
}

static bool read_split_cache(const char *date, day_split_t &out)
{
    char path[64];
    split_path(date, path, sizeof(path));
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > 8192) { fclose(f); return false; }
    std::string buf((size_t)len, '\0');
    size_t got = fread(&buf[0], 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len) return false;

    cJSON *root = cJSON_Parse(buf.c_str());
    if (!root) return false;
    cJSON *solar  = cJSON_GetObjectItemCaseSensitive(root, "solar_kwh");
    cJSON *gridk  = cJSON_GetObjectItemCaseSensitive(root, "grid_kwh");
    cJSON *ledger = cJSON_GetObjectItemCaseSensitive(root, "ledger");
    bool ok = cJSON_IsObject(solar) && cJSON_IsObject(gridk) && cJSON_IsObject(ledger);
    if (ok) {
        out.wh.clear();
        cJSON *c = nullptr;
        cJSON_ArrayForEach(c, solar) {
            if (!cJSON_IsNumber(c)) continue;
            cJSON *g = cJSON_GetObjectItemCaseSensitive(gridk, c->string);
            out.wh.push_back({ c->string, { c->valuedouble * 1000.0, cJSON_IsNumber(g) ? g->valuedouble * 1000.0 : 0.0 } });
        }
        cJSON *e  = cJSON_GetObjectItemCaseSensitive(ledger, "e_wh");
        cJSON *fr = cJSON_GetObjectItemCaseSensitive(ledger, "f");
        cJSON *kn = cJSON_GetObjectItemCaseSensitive(ledger, "known");
        out.end = battery_ledger_t{};
        out.end.soc = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(ledger, "soc"));
        if (cJSON_IsTrue(kn) && cJSON_IsNumber(e) && cJSON_IsNumber(fr)) {
            out.end.e_wh  = e->valuedouble;
            out.end.f     = fr->valuedouble;
            out.end.known = true;
        }
    }
    cJSON_Delete(root);
    return ok;
}

static void write_split_cache(const char *date, const day_split_t &ds)
{
    cJSON *root  = cJSON_CreateObject();
    cJSON *solar = cJSON_AddObjectToObject(root, "solar_kwh");
    cJSON *gridk = cJSON_AddObjectToObject(root, "grid_kwh");
    for (const auto &w : ds.wh) {
        cJSON_AddNumberToObject(solar, w.first.c_str(), w.second.first / 1000.0);
        cJSON_AddNumberToObject(gridk, w.first.c_str(), w.second.second / 1000.0);
    }
    cJSON *ledger = cJSON_AddObjectToObject(root, "ledger");
    cJSON_AddNumberToObject(ledger, "e_wh", ds.end.e_wh);
    cJSON_AddNumberToObject(ledger, "f", ds.end.f);
    cJSON_AddBoolToObject(ledger, "known", ds.end.known);
    cJSON_AddBoolToObject(ledger, "soc", ds.end.soc);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return;

    char path[64];
    split_path(date, path, sizeof(path));
    FILE *f = fopen(path, "wb");
    if (f) {
        fputs(json, f);
        fclose(f);
    }
    free(json);
}

// Like day_cost(): a finished day is computed once and cached, today is live.
static bool day_split(const char *date, bool is_today, const std::vector<stream_info_t> &streams,
                      const battery_ledger_t &start, day_split_t &out)
{
    if (!is_today && read_split_cache(date, out)) return true;
    if (!compute_day_split(date, streams, start, out)) return false;
    if (!is_today) write_split_cache(date, out);
    return true;
}

// The battery ledger at the end of the day before `date`, from its cached split.
// A day that was never split (the HEM was off at midnight) is split now, chaining
// back up to `depth` days, so the answer does not depend on what ran first.
// Unknown when there is no data to chain from.
static battery_ledger_t ledger_before(const char *date, int depth = 7)
{
    time_t t;
    if (!local_day_start(date, &t)) return battery_ledger_t{};
    struct tm d;
    localtime_r(&t, &d);
    d.tm_hour = 12; // midday avoids DST edge cases when normalising
    d.tm_mday -= 1;
    mktime(&d);
    char prev[11];
    strftime(prev, sizeof(prev), "%Y-%m-%d", &d);
    day_split_t ds;
    if (read_split_cache(prev, ds)) return ds.end;

    char path[96];
    snprintf(path, sizeof(path), "%s/grid-%s", SD_BASE, prev);
    if (depth <= 0 || access(path, F_OK) != 0) return battery_ledger_t{};
    battery_ledger_t before = ledger_before(prev, depth - 1);
    return day_split(prev, false, snapshot_streams(), before, ds) ? ds.end : battery_ledger_t{};
}

static void split_day_cached(const char *date)
{
    day_split_t ds;
    day_split(date, false, snapshot_streams(), ledger_before(date), ds);
}

char *node_power_logger_battery_source_json(void)
{
    time_t now = time(NULL);
    struct tm today;
    localtime_r(&now, &today);
    char date[11];
    strftime(date, sizeof(date), "%Y-%m-%d", &today);

    // Today's split, run from yesterday's closing ledger, leaves the ledger as it stands now.
    day_split_t ds;
    bool ok = compute_day_split(date, snapshot_streams(), ledger_before(date), ds);

    cJSON *root = cJSON_CreateObject();
    bool known = ok && ds.end.known && ds.end.e_wh > 0;
    cJSON_AddBoolToObject(root, "known", known);
    if (known) {
        double f = ds.end.f < 0.0 ? 0.0 : (ds.end.f > 1.0 ? 1.0 : ds.end.f);
        cJSON_AddNumberToObject(root, "solar_pct", f * 100.0);
        cJSON_AddNumberToObject(root, "grid_pct", (1.0 - f) * 100.0);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json; // caller must free
}

char *node_power_logger_daily_energy_json(int days)
{
    std::vector<stream_info_t> streams = snapshot_streams();

    cJSON *root  = cJSON_CreateObject();
    cJSON *nodes = cJSON_AddArrayToObject(root, "nodes");
    for (const auto &s : streams) {
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "id",   s.graph_id.c_str());
        cJSON_AddStringToObject(obj, "role", s.role);
        cJSON_AddItemToArray(nodes, obj);
    }

    cJSON *arr = cJSON_AddArrayToObject(root, "days");
    time_t now = time(NULL);
    struct tm today;
    localtime_r(&now, &today);

    bool     have_currency = false;
    uint16_t currency = 0;
    battery_ledger_t ledger; // seeded from the day before the window on the first iteration
    bool ledger_seeded = false;

    // Oldest first. Step the calendar day via mktime so DST changes are handled.
    for (int back = days - 1; back >= 0; back--) {
        struct tm d = today;
        d.tm_hour = 12; // midday avoids DST edge cases when normalising
        d.tm_mday -= back;
        mktime(&d);
        char date[11];
        strftime(date, sizeof(date), "%Y-%m-%d", &d);

        cJSON *day = cJSON_CreateObject();
        cJSON_AddStringToObject(day, "date", date);
        cJSON *kwh = cJSON_AddObjectToObject(day, "kwh");

        for (const auto &s : streams) {
            char hourly[96], minute[96];
            if (s.is_grid) {
                snprintf(hourly, sizeof(hourly), "%s/grid-hourly-%s", SD_BASE, date);
                snprintf(minute, sizeof(minute), "%s/grid-%s", SD_BASE, date);
            } else {
                snprintf(hourly, sizeof(hourly), "%s/nodeh-%s-%s", SD_BASE, s.graph_id.c_str(), date);
                snprintf(minute, sizeof(minute), "%s/node-%s-%s", SD_BASE, s.graph_id.c_str(), date);
            }
            double v;
            if (file_kwh(hourly, minute, &v))
                cJSON_AddNumberToObject(kwh, s.graph_id.c_str(), v);
        }

        // Cost in major currency units (the stored money value / 10^decimals),
        // so days priced with different precision still add up. Omitted without a tariff.
        day_cost_t dc;
        if (day_cost(date, back == 0, streams, dc, true)) {
            double scale = pow(10.0, dc.decimals);
            cJSON *cost = cJSON_AddObjectToObject(day, "cost");
            for (const auto &c : dc.cost) cJSON_AddNumberToObject(cost, c.first.c_str(), c.second / scale);
            have_currency = true;
            currency = dc.currency;
        }

        // Solar vs grid energy per load, chaining the battery ledger day to day.
        if (!ledger_seeded) {
            ledger = ledger_before(date);
            ledger_seeded = true;
        }
        day_split_t ds;
        if (day_split(date, back == 0, streams, ledger, ds)) {
            cJSON *solar = cJSON_AddObjectToObject(day, "solar_kwh");
            cJSON *gridk = cJSON_AddObjectToObject(day, "grid_kwh");
            for (const auto &w : ds.wh) {
                cJSON_AddNumberToObject(solar, w.first.c_str(), w.second.first / 1000.0);
                cJSON_AddNumberToObject(gridk, w.first.c_str(), w.second.second / 1000.0);
            }
            ledger = ds.end;
        } else {
            ledger = battery_ledger_t{}; // no grid data: the battery's history is lost
        }
        cJSON_AddItemToArray(arr, day);
    }

    if (have_currency) cJSON_AddNumberToObject(root, "currency", currency);

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json; // caller must free
}


// ---- Diagnostics (served by the MCP endpoint) -------------------------------

// Today as YYYY-MM-DD in local time.
static void today_str(char *buf, size_t len)
{
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(buf, len, "%Y-%m-%d", &tm_now);
}

// The local date `offset` days from `date`.
static bool date_add(const char *date, int offset, char *buf, size_t len)
{
    time_t t;
    if (!local_day_start(date, &t)) return false;
    struct tm d;
    localtime_r(&t, &d);
    d.tm_hour = 12; // midday avoids DST edge cases when normalising
    d.tm_mday += offset;
    mktime(&d);
    strftime(buf, len, "%Y-%m-%d", &d);
    return true;
}

static void add_ledger(cJSON *obj, const battery_ledger_t &l)
{
    bool known = l.known && l.e_wh > 0;
    cJSON_AddBoolToObject(obj, "known", known);
    if (!known) return;
    double f = l.f < 0.0 ? 0.0 : (l.f > 1.0 ? 1.0 : l.f);
    cJSON_AddNumberToObject(obj, "solar_pct", round(f * 1000.0) / 10.0);
    cJSON_AddNumberToObject(obj, "grid_pct", round((1.0 - f) * 1000.0) / 10.0);
    // State-of-charge days hold usable charge in half-percent; older days hold Wh.
    cJSON_AddNumberToObject(obj, "held", round(l.soc ? l.e_wh * 5.0 : l.e_wh * 10.0) / 10.0);
}

char *node_power_logger_battery_mix_trace_json(const char *date_str)
{
    char date[16], today[11];
    today_str(today, sizeof(today));
    time_t day_start;
    if (!sanitize_token(date_str, date, sizeof(date)) || strlen(date) != 10 ||
        !local_day_start(date, &day_start) || strcmp(date, today) > 0)
        return nullptr;

    battery_ledger_t start = ledger_before(date);
    day_split_t ds;
    std::vector<battery_ledger_t> hourly;
    bool ok = compute_day_split(date, snapshot_streams(), start, ds, &hourly);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "date", date);
    cJSON_AddBoolToObject(root, "has_data", ok);
    if (ok) {
        bool anchored = ds.end.soc;
        cJSON_AddBoolToObject(root, "anchored_to_soc", anchored);
        cJSON_AddStringToObject(root, "held_unit", anchored ? "percent of capacity above the 10% reserve" : "Wh");
        cJSON_AddStringToObject(root, "start_held_unit", start.soc ? "percent of capacity above the 10% reserve" : "Wh");
        add_ledger(cJSON_AddObjectToObject(root, "start"), start);

        // Hours still to come today would only repeat the latest ledger.
        size_t hours = hourly.size();
        if (strcmp(date, today) == 0) {
            long elapsed = ((long)time(NULL) - (long)day_start) / 3600 + 1;
            if (elapsed >= 0 && (size_t)elapsed < hours) hours = (size_t)elapsed;
        }
        cJSON *arr = cJSON_AddArrayToObject(root, "hours");
        for (size_t h = 0; h < hours; h++) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "hour", (double)h);
            add_ledger(o, hourly[h]);
            cJSON_AddItemToArray(arr, o);
        }
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json; // caller must free
}

char *node_power_logger_series_json(const char *node_id, const char *date_str, bool hourly)
{
    char node[40], date[16];
    time_t day_start;
    if (!sanitize_token(node_id, node, sizeof(node)) ||
        !sanitize_token(date_str, date, sizeof(date)) || strlen(date) != 10 ||
        !local_day_start(date, &day_start))
        return nullptr;

    const char *role = nullptr;
    for (const auto &st : snapshot_streams())
        if (st.graph_id == node) { role = st.role; break; }

    char path[96];
    if (role && strcmp(role, "grid") == 0)
        snprintf(path, sizeof(path), "%s/grid-%s", SD_BASE, date);
    else
        snprintf(path, sizeof(path), "%s/node-%s-%s", SD_BASE, node, date);

    // INT32_MIN marks a minute with no record.
    std::vector<int32_t> mw;
    bool ok = load_minutes(path, day_start, mw, INT32_MIN);
    int last = -1;
    if (ok)
        for (int i = 0; i < kMaxDayMinutes; i++) if (mw[i] != INT32_MIN) last = i;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "node_id", node);
    cJSON_AddStringToObject(root, "date", date);
    if (role) cJSON_AddStringToObject(root, "role", role);
    cJSON_AddStringToObject(root, "resolution", hourly ? "hourly" : "minute");
    cJSON_AddNumberToObject(root, "step_minutes", hourly ? 60 : 1);
    cJSON_AddStringToObject(root, "first_sample", "00:00 local");
    cJSON_AddStringToObject(root, "sign",
        "raw meter reading: + = power into the node (grid import, load consumption, battery charging); "
        "- = power out of it (grid export, inverter generating, battery discharging)");
    cJSON *arr = cJSON_AddArrayToObject(root, "power_w"); // null = no reading
    int step = hourly ? 60 : 1;
    for (int i = 0; i <= last; i += step) {
        int64_t sum = 0;
        int n = 0;
        for (int j = i; j < i + step && j <= last; j++)
            if (mw[j] != INT32_MIN) { sum += mw[j]; n++; }
        if (n == 0) cJSON_AddItemToArray(arr, cJSON_CreateNull());
        else        cJSON_AddItemToArray(arr, cJSON_CreateNumber(round((double)sum / n / 100.0) / 10.0));
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json; // caller must free
}

char *node_power_logger_stream_health_json(void)
{
    struct item_t { std::string graph_id; const char *role; bool is_grid; uint64_t node_id; uint16_t endpoint_id; };
    std::vector<item_t> items;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (const auto &st : s_streams)
        items.push_back({ st.graph_id, st.role, st.is_grid, st.node_id, st.endpoint_id });
    xSemaphoreGive(s_mutex);

    std::vector<ValueCacheEntry> snap = ValueCache::instance().snapshot();
    char today[11];
    today_str(today, sizeof(today));
    time_t day_start = 0;
    local_day_start(today, &day_start);
    time_t now = time(NULL);
    int elapsed = (int)((now - day_start) / 60); // complete minutes so far today
    if (elapsed > kMaxDayMinutes) elapsed = kMaxDayMinutes;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "date", today);
    cJSON_AddNumberToObject(root, "minutes_elapsed_today", elapsed);
    cJSON *arr = cJSON_AddArrayToObject(root, "streams");
    for (const auto &it : items) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", it.graph_id.c_str());
        cJSON_AddStringToObject(o, "role", it.role);

        // Age of the last Matter report. The logger keeps recording the cached
        // value, so a stale report means the minute data is frozen, not missing.
        bool reported = false;
        for (const auto &v : snap) {
            if (!v.valid || v.node_id != it.node_id || v.endpoint_id != it.endpoint_id) continue;
            if (v.cluster_id != EPM_CLUSTER_ID || v.attribute_id != EPM_ACTIVE_POWER_ATTR) continue;
            cJSON_AddNumberToObject(o, "last_report_age_s", (double)((long)now - (long)v.last_update_unix));
            cJSON_AddNumberToObject(o, "last_power_w", v.value / 1000.0);
            reported = true;
            break;
        }
        if (!reported) cJSON_AddNullToObject(o, "last_report_age_s");

        char path[96];
        if (it.is_grid) snprintf(path, sizeof(path), "%s/grid-%s", SD_BASE, today);
        else            snprintf(path, sizeof(path), "%s/node-%s-%s", SD_BASE, it.graph_id.c_str(), today);
        std::vector<int32_t> mw;
        int records = 0, gap = 0, largest = 0;
        if (load_minutes(path, day_start, mw, INT32_MIN)) {
            for (int i = 0; i < elapsed; i++) {
                if (mw[i] != INT32_MIN) { records++; gap = 0; }
                else if (++gap > largest) largest = gap;
            }
        } else {
            largest = elapsed;
        }
        cJSON_AddNumberToObject(o, "minutes_recorded_today", records);
        cJSON_AddNumberToObject(o, "largest_gap_minutes", largest);
        cJSON_AddItemToArray(arr, o);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json; // caller must free
}

int node_power_logger_recompute_from(const char *date_str)
{
    char date[16], today[11];
    today_str(today, sizeof(today));
    time_t t;
    if (!sanitize_token(date_str, date, sizeof(date)) || strlen(date) != 10 || !local_day_start(date, &t))
        return -1;

    // Later days chain their battery ledger from earlier ones, so every cache
    // from `date` up to yesterday is dropped before any is rebuilt.
    std::vector<std::string> days;
    char d[11];
    snprintf(d, sizeof(d), "%s", date);
    while (strcmp(d, today) < 0) {
        if (days.size() >= 60) return -1;
        days.push_back(d);
        char path[64];
        cost_path(d, path, sizeof(path));
        unlink(path);
        split_path(d, path, sizeof(path));
        unlink(path);
        char next[11];
        if (!date_add(d, 1, next, sizeof(next))) return -1;
        snprintf(d, sizeof(d), "%s", next);
    }
    if (days.empty()) return 0;

    std::vector<stream_info_t> streams = snapshot_streams();
    battery_ledger_t ledger = ledger_before(days[0].c_str());
    int done = 0;
    for (const auto &day : days) {
        day_cost_t dc;
        day_cost(day.c_str(), false, streams, dc, true);
        day_split_t ds;
        if (day_split(day.c_str(), false, streams, ledger, ds)) {
            ledger = ds.end;
            done++;
        } else {
            ledger = battery_ledger_t{}; // no grid data: the battery's history is lost
        }
    }
    return done;
}
