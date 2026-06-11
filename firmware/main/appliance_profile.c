#include "appliance_profile.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "cJSON.h"

#include "power_logger.h"          // power_record_t
#include "managers/node_manager.h" // node_manager_get_all_json

static const char *TAG = "appliance_profile";

#define LFS_BASE  "/littlefs"

static const char *CONSUMER_UNIT_ID = "consumer_unit";
static const char *GRID_NODE_ID     = "grid_meter";

// --- Standby estimation -----------------------------------------------------
// A coarse power histogram over the whole window lets us find the modal draw
// (the idle level the appliance sits at most of the time) without storing every
// sample. Anything above STANDBY_MAX_W is lumped into the top bin — it is "on",
// not standby.
#define STANDBY_BIN_W   5                              // histogram bin width (W)
#define STANDBY_MAX_W   2000                           // histogram ceiling (W)
#define STANDBY_NBINS   (STANDBY_MAX_W / STANDBY_BIN_W)

// --- Cycle detection (hysteresis + gap tolerance) ---------------------------
// A "program" runs while the draw stays above standby. Thresholds are relative
// to the learned standby so a 5 W phone charger and a 2 kW kettle both work.
#define ON_MARGIN_MW       (30 * 1000) // must exceed standby by >=30 W to start
#define OFF_MARGIN_MW      (15 * 1000) // ends when below standby+15 W (hysteresis)
#define ON_REL_NUM         3           // ...or standby + 30% of (peak-standby),
#define ON_REL_DEN         10          //    whichever start threshold is larger
#define GAP_TOLERANCE_MIN  5           // tolerate this many low minutes mid-cycle
#define MIN_CYCLE_MIN      5           // discard runs shorter than this (noise)

#define MAX_APPLIANCES     32          // cap when enumerating the topology graph

// ---------------------------------------------------------------------------

// Copy a graph-id/date token into out only if filesystem-safe ([A-Za-z0-9_-],
// length 1..32). Same guard node_power_logger uses before touching /littlefs.
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

static const char *json_str(cJSON *obj, const char *key)
{
    cJSON *j = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(j) ? j->valuestring : NULL;
}

static void date_for_offset(time_t now, int days_ago, char *date, size_t len)
{
    struct tm tm;
    localtime_r(&now, &tm);
    tm.tm_mday -= days_ago;
    tm.tm_hour = 12;   // midday avoids DST edge wobble when normalising
    tm.tm_min  = 0;
    tm.tm_sec  = 0;
    mktime(&tm);
    strftime(date, len, "%Y-%m-%d", &tm);
}

// ---------------------------------------------------------------------------
// Cycle aggregation: running mean/stddev over the per-cycle mean power and the
// per-cycle length, accumulated across every day in the window.
typedef struct {
    uint32_t count;
    double   sum_pow, sumsq_pow; // per-cycle mean power (mW)
    double   sum_len, sumsq_len; // per-cycle length (minutes)
} cycle_stats_t;

static void record_cycle(cycle_stats_t *st, int64_t cyc_sum_mw, uint32_t cyc_len)
{
    if (cyc_len < MIN_CYCLE_MIN)
        return;
    double mean_pow = (double)cyc_sum_mw / (double)cyc_len;
    st->count++;
    st->sum_pow   += mean_pow;
    st->sumsq_pow += mean_pow * mean_pow;
    st->sum_len   += cyc_len;
    st->sumsq_len += (double)cyc_len * (double)cyc_len;
}

// Walk one day's per-minute file in time order and detect cycles. Records are
// treated as consecutive minutes (missing minutes within a day are simply not
// seen — acceptable at minute granularity). Returns true if the file existed
// with at least one record.
static bool scan_day(const char *graph_id, const char *date,
                     int32_t on_threshold, int32_t off_threshold,
                     cycle_stats_t *st)
{
    char path[96];
    snprintf(path, sizeof(path), "%s/node-%s-%s", LFS_BASE, graph_id, date);
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;

    bool     have_data = false;
    bool     in_cycle  = false;
    int64_t  cyc_sum   = 0;  // committed active-minute power sum (mW)
    uint32_t cyc_len   = 0;  // committed active minutes
    int64_t  pend_sum  = 0;  // tentative gap minutes, folded in only if it resumes
    uint32_t pend_len  = 0;
    int      gap       = 0;

    power_record_t rec;
    while (fread(&rec, sizeof(rec), 1, f) == 1) {
        have_data = true;
        int32_t p = rec.power_mw;

        if (!in_cycle) {
            if (p >= on_threshold) {
                in_cycle = true;
                cyc_sum  = p;
                cyc_len  = 1;
                pend_sum = 0; pend_len = 0; gap = 0;
            }
            continue;
        }

        if (p >= off_threshold) {
            // Still running — fold any tolerated gap minutes back in.
            cyc_sum += pend_sum + p;
            cyc_len += pend_len + 1;
            pend_sum = 0; pend_len = 0; gap = 0;
        } else {
            gap++;
            if (gap > GAP_TOLERANCE_MIN) {
                record_cycle(st, cyc_sum, cyc_len); // trailing gap discarded
                in_cycle = false;
                cyc_sum = 0; cyc_len = 0;
                pend_sum = 0; pend_len = 0; gap = 0;
            } else {
                pend_sum += p; pend_len++;
            }
        }
    }
    if (in_cycle)
        record_cycle(st, cyc_sum, cyc_len);

    fclose(f);
    return have_data;
}

int appliance_profile_train(const char *graph_id, int window_days)
{
    char id[40];
    if (!sanitize_token(graph_id, id, sizeof(id))) {
        ESP_LOGW(TAG, "Skipping unsafe graph id");
        return 0;
    }

    time_t now = time(NULL);

    // Pass 1: build the standby histogram and find the peak draw.
    uint32_t hist[STANDBY_NBINS] = {0};
    int32_t  peak_mw = 0;
    for (int d = 1; d <= window_days; d++) {
        char date[11];
        date_for_offset(now, d, date, sizeof(date));

        char path[96];
        snprintf(path, sizeof(path), "%s/node-%s-%s", LFS_BASE, id, date);
        FILE *f = fopen(path, "rb");
        if (!f)
            continue;

        power_record_t rec;
        while (fread(&rec, sizeof(rec), 1, f) == 1) {
            int32_t p = rec.power_mw;
            if (p > peak_mw) peak_mw = p;
            int bin = (p <= 0) ? 0 : (p / 1000) / STANDBY_BIN_W;
            if (bin < 0) bin = 0;
            if (bin >= STANDBY_NBINS) bin = STANDBY_NBINS - 1;
            hist[bin]++;
        }
        fclose(f);
    }

    int mode_bin = 0;
    for (int b = 1; b < STANDBY_NBINS; b++)
        if (hist[b] > hist[mode_bin]) mode_bin = b;
    // Centre of the modal band, in mW.
    int32_t standby_mw = (mode_bin * STANDBY_BIN_W + STANDBY_BIN_W / 2) * 1000;

    // Thresholds derived from standby (with a relative floor scaled to the peak).
    int32_t rel = (int32_t)(((int64_t)(peak_mw - standby_mw) * ON_REL_NUM) / ON_REL_DEN);
    int32_t on_margin = ON_MARGIN_MW > rel ? ON_MARGIN_MW : rel;
    int32_t on_threshold  = standby_mw + on_margin;
    int32_t off_threshold = standby_mw + OFF_MARGIN_MW;

    // Pass 2: detect cycles day by day.
    cycle_stats_t st = {0};
    uint16_t days_with_data = 0;
    for (int d = 1; d <= window_days; d++) {
        char date[11];
        date_for_offset(now, d, date, sizeof(date));
        if (scan_day(id, date, on_threshold, off_threshold, &st))
            days_with_data++;
    }

    appliance_profile_t prof;
    memset(&prof, 0, sizeof(prof));
    prof.magic          = APPLIANCE_PROFILE_MAGIC;
    prof.version        = APPLIANCE_PROFILE_VERSION;
    prof.window_days    = (uint16_t)window_days;
    prof.trained_unix   = (uint32_t)now;
    prof.standby_mw     = standby_mw;
    prof.program_count  = (uint16_t)st.count;
    prof.days_with_data = days_with_data;

    if (st.count > 0) {
        double mean_pow = st.sum_pow / st.count;
        double var_pow  = st.sumsq_pow / st.count - mean_pow * mean_pow;
        double mean_len = st.sum_len / st.count;
        double var_len  = st.sumsq_len / st.count - mean_len * mean_len;
        prof.avg_program_power_mw = (int32_t)lround(mean_pow);
        prof.std_program_power_mw = (int32_t)lround(sqrt(var_pow > 0 ? var_pow : 0));
        prof.avg_program_len_min  = (uint32_t)lround(mean_len);
        prof.std_program_len_min  = (uint32_t)lround(sqrt(var_len > 0 ? var_len : 0));
    }

    char ppath[96];
    snprintf(ppath, sizeof(ppath), "%s/profile-%s", LFS_BASE, id);
    FILE *out = fopen(ppath, "wb");
    if (!out) {
        ESP_LOGE(TAG, "Cannot write %s", ppath);
        return st.count;
    }
    fwrite(&prof, sizeof(prof), 1, out);
    fclose(out);

    ESP_LOGI(TAG,
             "Profiled %s: standby %d W, %u program(s), avg %d W over %u min (%u day(s) w/ data)",
             id, prof.standby_mw / 1000, prof.program_count,
             prof.avg_program_power_mw / 1000, prof.avg_program_len_min, days_with_data);
    return st.count;
}

// ---------------------------------------------------------------------------
// Topology enumeration: an appliance is any node wired to the consumer unit
// whose CU handle is not the grid, solar or battery (and not the grid meter).
typedef void (*appliance_visit_fn)(const char *graph_id, const char *name, void *ctx);

static bool handle_is_appliance(const char *handle)
{
    if (!handle) return true; // unknown handle: assume appliance (grid is excluded by id)
    if (strncmp(handle, "grid", 4) == 0)    return false;
    if (strncmp(handle, "solar", 5) == 0)   return false;
    if (strncmp(handle, "battery", 7) == 0) return false;
    return true;
}

// Find a node's user-facing name from its settings (name, then label). Returns
// NULL if the node or a usable label isn't present.
static const char *node_display_name(cJSON *nodes, const char *id)
{
    cJSON *n = NULL;
    cJSON_ArrayForEach(n, nodes) {
        const char *nid = json_str(n, "id");
        if (!nid || strcmp(nid, id) != 0) continue;
        cJSON *settings = cJSON_GetObjectItemCaseSensitive(n, "settings");
        if (settings) {
            const char *nm = json_str(settings, "name");
            if (nm && nm[0]) return nm;
            const char *lb = json_str(settings, "label");
            if (lb && lb[0]) return lb;
        }
        return NULL;
    }
    return NULL;
}

// A "distribution node" is a board that loads hang off: the main consumer unit
// or any sub consumer unit. Loads wired to either count as appliances; an edge
// between two distribution nodes (the CU -> sub-CU feed) does not, so the sub-CU
// board itself is never enumerated. Works for arbitrarily nested sub-boards.
static bool is_distribution_node(cJSON *nodes, const char *id)
{
    if (!id) return false;
    if (strcmp(id, CONSUMER_UNIT_ID) == 0) return true;
    cJSON *n = NULL;
    cJSON_ArrayForEach(n, nodes) {
        const char *nid = json_str(n, "id");
        if (!nid || strcmp(nid, id) != 0) continue;
        cJSON *settings = cJSON_GetObjectItemCaseSensitive(n, "settings");
        const char *t = settings ? json_str(settings, "type") : NULL;
        return t && strcmp(t, "subConsumerUnit") == 0;
    }
    return false;
}

static void enumerate_appliances(appliance_visit_fn fn, void *ctx)
{
    char *raw = node_manager_get_all_json();
    if (!raw) return;
    cJSON *root = cJSON_Parse(raw);
    free(raw);
    if (!root) return;

    cJSON *edges = cJSON_GetObjectItemCaseSensitive(root, "edges");
    cJSON *nodes = cJSON_GetObjectItemCaseSensitive(root, "nodes");

    char seen[MAX_APPLIANCES][40];
    int  seen_n = 0;

    cJSON *e = NULL;
    cJSON_ArrayForEach(e, edges) {
        const char *src = json_str(e, "source");
        const char *tgt = json_str(e, "target");
        if (!src || !tgt) continue;

        // The appliance is the non-distribution end of an edge that touches a
        // distribution node. Edges between two distribution nodes (CU -> sub-CU)
        // are the feed, not an appliance, so they are skipped.
        bool src_dist = is_distribution_node(nodes, src);
        bool tgt_dist = is_distribution_node(nodes, tgt);

        const char *other  = NULL;
        const char *handle = NULL;
        if (src_dist && !tgt_dist)      { other = tgt; handle = json_str(e, "sourceHandle"); }
        else if (tgt_dist && !src_dist) { other = src; handle = json_str(e, "targetHandle"); }
        else continue;

        if (strcmp(other, GRID_NODE_ID) == 0) continue;
        if (!handle_is_appliance(handle))     continue;

        char clean[40];
        if (!sanitize_token(other, clean, sizeof(clean))) continue;

        bool dup = false;
        for (int i = 0; i < seen_n; i++)
            if (strcmp(seen[i], clean) == 0) { dup = true; break; }
        if (dup) continue;
        if (seen_n >= MAX_APPLIANCES) break;
        strcpy(seen[seen_n++], clean);

        fn(clean, node_display_name(nodes, other), ctx);
    }

    cJSON_Delete(root);
}

static void train_one(const char *graph_id, const char *name, void *ctx)
{
    (void)name;
    int window_days = *(int *)ctx;
    appliance_profile_train(graph_id, window_days);
}

void appliance_profile_train_all(int window_days)
{
    enumerate_appliances(train_one, &window_days);
}

// Collector for appliance_enumerate(): copies each appliance's id and name into
// caller-supplied parallel arrays, up to the cap.
typedef struct {
    char (*ids)[APPLIANCE_ID_MAX_LEN];
    char (*names)[APPLIANCE_NAME_MAX_LEN];
    size_t max;
    size_t n;
} collect_ctx_t;

static void collect_one(const char *graph_id, const char *name, void *ctx)
{
    collect_ctx_t *c = (collect_ctx_t *)ctx;
    if (c->n >= c->max) return;
    snprintf(c->ids[c->n], APPLIANCE_ID_MAX_LEN, "%s", graph_id);
    snprintf(c->names[c->n], APPLIANCE_NAME_MAX_LEN, "%s", (name && name[0]) ? name : graph_id);
    c->n++;
}

size_t appliance_enumerate(char ids[][APPLIANCE_ID_MAX_LEN],
                           char names[][APPLIANCE_NAME_MAX_LEN],
                           size_t max)
{
    collect_ctx_t c = { ids, names, max, 0 };
    enumerate_appliances(collect_one, &c);
    return c.n;
}

// ---------------------------------------------------------------------------
// JSON readers.
static bool load_profile(const char *id, appliance_profile_t *prof)
{
    char path[96];
    snprintf(path, sizeof(path), "%s/profile-%s", LFS_BASE, id);
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    bool ok = (fread(prof, sizeof(*prof), 1, f) == 1);
    fclose(f);
    if (!ok || prof->magic != APPLIANCE_PROFILE_MAGIC ||
        prof->version != APPLIANCE_PROFILE_VERSION)
        return false;
    return true;
}

bool appliance_profile_load(const char *graph_id, appliance_profile_t *out)
{
    char id[40];
    if (!out || !sanitize_token(graph_id, id, sizeof(id)))
        return false;
    return load_profile(id, out);
}

// Build a cJSON object for one appliance (always returns an object; "trained"
// is false when no valid profile is on disk).
static cJSON *profile_to_cjson(const char *id)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "graph_id", id);

    appliance_profile_t prof;
    if (!load_profile(id, &prof)) {
        cJSON_AddBoolToObject(o, "trained", false);
        return o;
    }

    cJSON_AddBoolToObject(o, "trained", true);
    cJSON_AddNumberToObject(o, "standby_w",           prof.standby_mw / 1000.0);
    cJSON_AddNumberToObject(o, "avg_program_power_w", prof.avg_program_power_mw / 1000.0);
    cJSON_AddNumberToObject(o, "std_program_power_w", prof.std_program_power_mw / 1000.0);
    cJSON_AddNumberToObject(o, "avg_program_len_min", prof.avg_program_len_min);
    cJSON_AddNumberToObject(o, "std_program_len_min", prof.std_program_len_min);
    cJSON_AddNumberToObject(o, "program_count",       prof.program_count);
    cJSON_AddNumberToObject(o, "days_with_data",      prof.days_with_data);
    cJSON_AddNumberToObject(o, "trained_unix",        (double)prof.trained_unix);
    cJSON_AddNumberToObject(o, "window_days",         prof.window_days);
    return o;
}

char *appliance_profile_json(const char *graph_id)
{
    char id[40];
    if (!sanitize_token(graph_id, id, sizeof(id))) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "trained", false);
        char *json = cJSON_PrintUnformatted(o);
        cJSON_Delete(o);
        return json;
    }
    cJSON *o = profile_to_cjson(id);
    char *json = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return json; // caller must free
}

static void append_one(const char *graph_id, const char *name, void *ctx)
{
    (void)name;
    cJSON *arr = (cJSON *)ctx;
    cJSON_AddItemToArray(arr, profile_to_cjson(graph_id));
}

char *appliance_profile_all_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr  = cJSON_AddArrayToObject(root, "appliances");
    enumerate_appliances(append_one, arr);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json; // caller must free
}
