#include "appliance_profile.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <stddef.h>

#include "esp_log.h"
#include "cJSON.h"

#include "power_logger.h"          // power_record_t
#include "managers/node_manager.h" // node_manager_get_all_json
#include "appliance_state.h"         // state log, program catalogue, live state

static const char *TAG = "appliance_profile";

#define SD_BASE  "/sdcard"

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
// length 1..32). Same guard node_power_logger uses before touching /sdcard.
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

// Formats the local date days_ago days before now and returns its weekday
// (tm_wday, 0 = Sunday).
static int date_for_offset(time_t now, int days_ago, char *date, size_t len)
{
    struct tm tm;
    localtime_r(&now, &tm);
    tm.tm_mday -= days_ago;
    tm.tm_hour = 12;   // midday avoids DST edge wobble when normalising
    tm.tm_min  = 0;
    tm.tm_sec  = 0;
    mktime(&tm);
    strftime(date, len, "%Y-%m-%d", &tm);
    return tm.tm_wday;
}

// ---------------------------------------------------------------------------
// Cycle aggregation: running mean/stddev over the per-cycle mean power and the
// per-cycle length, accumulated across every day in the window. The dow_* arrays
// split the cycle count and energy by weekday (tm_wday).
typedef struct {
    uint32_t count;
    double   sum_pow, sumsq_pow; // per-cycle mean power (mW)
    double   sum_len, sumsq_len; // per-cycle length (minutes)
    double   sum_energy_mwh;
    uint32_t dow_runs[7];
    double   dow_energy_mwh[7];
} cycle_stats_t;

#define MINUTES_PER_DAY 1440

// Everything one training run accumulates, heap-allocated to keep the nightly
// job's stack small. all covers every cycle; programs[i] only the cycles tagged
// with catalogue program i. op/prog hold the day being scanned's state log by
// minute of day.
typedef struct {
    cycle_stats_t all;
    cycle_stats_t programs[APPLIANCE_PROGRAM_MAX];
    uint8_t       op[MINUTES_PER_DAY];
    uint8_t       prog[MINUTES_PER_DAY];
} train_t;

static void add_cycle(cycle_stats_t *st, int wday, int64_t cyc_sum_mw, uint32_t cyc_len)
{
    double mean_pow = (double)cyc_sum_mw / (double)cyc_len;
    double energy   = (double)cyc_sum_mw / 60.0; // mW-minutes -> mWh
    st->count++;
    st->sum_pow   += mean_pow;
    st->sumsq_pow += mean_pow * mean_pow;
    st->sum_len   += cyc_len;
    st->sumsq_len += (double)cyc_len * (double)cyc_len;
    st->sum_energy_mwh += energy;
    st->dow_runs[wday]++;
    st->dow_energy_mwh[wday] += energy;
}

static void record_cycle(train_t *t, int wday, int64_t cyc_sum_mw, uint32_t cyc_len, uint8_t program_id)
{
    if (cyc_len < MIN_CYCLE_MIN)
        return;
    add_cycle(&t->all, wday, cyc_sum_mw, cyc_len);
    if (program_id < APPLIANCE_PROGRAM_MAX)
        add_cycle(&t->programs[program_id], wday, cyc_sum_mw, cyc_len);
}

// Mean and stddev of a stats accumulator's per-cycle power and length.
static void finish_stats(const cycle_stats_t *st, int32_t *avg_pow, int32_t *std_pow,
                         uint32_t *avg_len, uint32_t *std_len)
{
    double mean_pow = st->sum_pow / st->count;
    double var_pow  = st->sumsq_pow / st->count - mean_pow * mean_pow;
    double mean_len = st->sum_len / st->count;
    double var_len  = st->sumsq_len / st->count - mean_len * mean_len;
    *avg_pow = (int32_t)lround(mean_pow);
    *std_pow = (int32_t)lround(sqrt(var_pow > 0 ? var_pow : 0));
    *avg_len = (uint32_t)lround(mean_len);
    *std_len = (uint32_t)lround(sqrt(var_len > 0 ? var_len : 0));
}

static int minute_of_day(uint32_t unix_minute)
{
    time_t t = (time_t)unix_minute;
    struct tm tm;
    localtime_r(&t, &tm);
    return tm.tm_hour * 60 + tm.tm_min;
}

// Load a day's state log into t->op / t->prog by minute of day. Returns true if
// the device was reachable for at least one minute, i.e. the day's cycles can be
// bounded by its Operational State.
static bool load_state_day(const char *graph_id, const char *date, train_t *t)
{
    memset(t->op, APPLIANCE_UNKNOWN, sizeof(t->op));
    memset(t->prog, APPLIANCE_UNKNOWN, sizeof(t->prog));

    char path[96];
    snprintf(path, sizeof(path), "%s/state-%s-%s", SD_BASE, graph_id, date);
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;

    bool usable = false;
    appliance_state_record_t rec;
    while (fread(&rec, sizeof(rec), 1, f) == 1) {
        if (!(rec.flags & APPLIANCE_STATE_FLAG_REACHABLE) || rec.op_state == APPLIANCE_UNKNOWN)
            continue;
        int m = minute_of_day(rec.unix_minute);
        t->op[m]   = rec.op_state;
        t->prog[m] = rec.program_id;
        usable = true;
    }
    fclose(f);
    return usable;
}

// State logged for a power minute. The power and state records of a minute come
// from the same flush but their timestamps can straddle a minute boundary, so a
// minute with no state borrows a neighbour's.
static int state_index(const train_t *t, uint32_t unix_minute)
{
    int m = minute_of_day(unix_minute);
    if (t->op[m] != APPLIANCE_UNKNOWN) return m;
    if (m > 0 && t->op[m - 1] != APPLIANCE_UNKNOWN) return m - 1;
    if (m + 1 < MINUTES_PER_DAY && t->op[m + 1] != APPLIANCE_UNKNOWN) return m + 1;
    return -1;
}

static bool op_is_running(uint8_t op)
{
    return op == APPLIANCE_OP_RUNNING || op == APPLIANCE_OP_PAUSED;
}

// The program a cycle ran: the one logged for most of its minutes.
static uint8_t winning_program(const uint16_t votes[APPLIANCE_PROGRAM_MAX])
{
    uint8_t best = APPLIANCE_UNKNOWN;
    uint16_t best_votes = 0;
    for (int i = 0; i < APPLIANCE_PROGRAM_MAX; i++)
        if (votes[i] > best_votes) { best_votes = votes[i]; best = (uint8_t)i; }
    return best;
}

// Walk one day's per-minute file in time order and detect cycles. Records are
// treated as consecutive minutes (missing minutes within a day are simply not
// seen — acceptable at minute granularity). wday is the day's weekday, which its
// cycles are counted under. Returns true if the file existed with at least one
// record.
//
// With use_state, t->op / t->prog hold the day's state log and a cycle is the
// stretch the device reports Running or Paused: it ends as soon as the device
// reports anything else, and minutes with no state are tolerated like low-power
// minutes. Each cycle is tagged with its program. Without it, cycles are found
// from the power thresholds and carry no program.
static bool scan_day(const char *graph_id, const char *date, int wday,
                     int32_t on_threshold, int32_t off_threshold,
                     bool use_state, train_t *t)
{
    char path[96];
    snprintf(path, sizeof(path), "%s/node-%s-%s", SD_BASE, graph_id, date);
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
    uint16_t votes[APPLIANCE_PROGRAM_MAX] = {0};

#define END_CYCLE() do {                                                        \
        record_cycle(t, wday, cyc_sum, cyc_len,                                 \
                     use_state ? winning_program(votes) : APPLIANCE_UNKNOWN);   \
        in_cycle = false;                                                       \
        cyc_sum = 0; cyc_len = 0;                                               \
        pend_sum = 0; pend_len = 0; gap = 0;                                    \
        memset(votes, 0, sizeof(votes));                                        \
    } while (0)

    power_record_t rec;
    while (fread(&rec, sizeof(rec), 1, f) == 1) {
        have_data = true;
        int32_t p = rec.power_mw;

        // Classify the minute: running, idle, or (state only) unknown.
        bool running, unknown = false;
        uint8_t program = APPLIANCE_UNKNOWN;
        if (use_state) {
            int m = state_index(t, rec.unix_minute);
            unknown = m < 0;
            running = !unknown && op_is_running(t->op[m]);
            if (running) program = t->prog[m];
        } else {
            running = p >= (in_cycle ? off_threshold : on_threshold);
        }

        if (!in_cycle) {
            if (running) {
                in_cycle = true;
                cyc_sum  = p;
                cyc_len  = 1;
                pend_sum = 0; pend_len = 0; gap = 0;
                if (program < APPLIANCE_PROGRAM_MAX) votes[program]++;
            }
            continue;
        }

        if (running) {
            // Still running — fold any tolerated gap minutes back in.
            cyc_sum += pend_sum + p;
            cyc_len += pend_len + 1;
            pend_sum = 0; pend_len = 0; gap = 0;
            if (program < APPLIANCE_PROGRAM_MAX) votes[program]++;
        } else if (use_state && !unknown) {
            END_CYCLE(); // the device says it has stopped
        } else {
            gap++;
            if (gap > GAP_TOLERANCE_MIN) {
                END_CYCLE(); // trailing gap discarded
            } else {
                pend_sum += p; pend_len++;
            }
        }
    }
    if (in_cycle)
        END_CYCLE();
#undef END_CYCLE

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
        snprintf(path, sizeof(path), "%s/node-%s-%s", SD_BASE, id, date);
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
    // Centre of the modal band, in mW. A mode in the lowest band (0..STANDBY_BIN_W)
    // means the appliance is effectively off when idle, so report a standby of 0.
    int32_t standby_mw = (mode_bin == 0) ? 0
                       : (mode_bin * STANDBY_BIN_W + STANDBY_BIN_W / 2) * 1000;

    // Thresholds derived from standby (with a relative floor scaled to the peak).
    int32_t rel = (int32_t)(((int64_t)(peak_mw - standby_mw) * ON_REL_NUM) / ON_REL_DEN);
    int32_t on_margin = ON_MARGIN_MW > rel ? ON_MARGIN_MW : rel;
    int32_t on_threshold  = standby_mw + on_margin;
    int32_t off_threshold = standby_mw + OFF_MARGIN_MW;

    // Pass 2: detect cycles day by day, bounded by the linked device's state on
    // days it was logged.
    train_t *t = calloc(1, sizeof(train_t));
    if (!t) {
        ESP_LOGE(TAG, "No memory to profile %s", id);
        return 0;
    }
    uint16_t days_with_data = 0;
    uint16_t dow_days[7] = {0};
    int      days_with_state = 0;
    for (int d = 1; d <= window_days; d++) {
        char date[11];
        int wday = date_for_offset(now, d, date, sizeof(date));
        bool use_state = load_state_day(id, date, t);
        if (scan_day(id, date, wday, on_threshold, off_threshold, use_state, t)) {
            days_with_data++;
            dow_days[wday]++;
            if (use_state) days_with_state++;
        }
    }
    const cycle_stats_t *st = &t->all;

    appliance_profile_t prof;
    memset(&prof, 0, sizeof(prof));
    prof.magic          = APPLIANCE_PROFILE_MAGIC;
    prof.version        = APPLIANCE_PROFILE_VERSION;
    prof.window_days    = (uint16_t)window_days;
    prof.trained_unix   = (uint32_t)now;
    prof.standby_mw     = standby_mw;
    prof.program_count  = (uint16_t)st->count;
    prof.days_with_data = days_with_data;
    for (int w = 0; w < 7; w++) {
        prof.dow_days[w]       = dow_days[w];
        prof.dow_runs[w]       = (uint16_t)st->dow_runs[w];
        prof.dow_energy_mwh[w] = (uint32_t)llround(st->dow_energy_mwh[w]);
    }

    if (st->count > 0)
        finish_stats(st, &prof.avg_program_power_mw, &prof.std_program_power_mw,
                     &prof.avg_program_len_min, &prof.std_program_len_min);

    // Keep the programs with the most runs.
    bool taken[APPLIANCE_PROGRAM_MAX] = {false};
    while (prof.program_entries < APPLIANCE_PROFILE_PROGRAMS) {
        int best = -1;
        for (int i = 0; i < APPLIANCE_PROGRAM_MAX; i++)
            if (!taken[i] && t->programs[i].count > 0 &&
                (best < 0 || t->programs[i].count > t->programs[best].count))
                best = i;
        if (best < 0) break;
        taken[best] = true;

        const cycle_stats_t *ps = &t->programs[best];
        appliance_program_profile_t *pp = &prof.programs[prof.program_entries++];
        pp->program_id     = (uint8_t)best;
        pp->runs           = (uint16_t)ps->count;
        pp->avg_energy_mwh = (uint32_t)llround(ps->sum_energy_mwh / ps->count);
        finish_stats(ps, &pp->avg_power_mw, &pp->std_power_mw, &pp->avg_len_min, &pp->std_len_min);
        for (int w = 0; w < 7; w++) pp->dow_runs[w] = (uint16_t)ps->dow_runs[w];
    }
    int cycles = (int)st->count;
    free(t);

    char ppath[96];
    snprintf(ppath, sizeof(ppath), "%s/profile-%s", SD_BASE, id);
    FILE *out = fopen(ppath, "wb");
    if (!out) {
        ESP_LOGE(TAG, "Cannot write %s: %s", ppath, strerror(errno));
        return cycles;
    }
    fwrite(&prof, sizeof(prof), 1, out);
    fclose(out);

    ESP_LOGI(TAG,
             "Profiled %s: standby %d W, %u program(s), avg %d W over %u min (%u day(s) w/ data, "
             "%d with device state, %u distinct program(s))",
             id, prof.standby_mw / 1000, prof.program_count,
             prof.avg_program_power_mw / 1000, prof.avg_program_len_min, days_with_data,
             days_with_state, prof.program_entries);
    return cycles;
}

// ---------------------------------------------------------------------------
// Topology enumeration: an appliance is any node wired to the consumer unit
// whose CU handle is not the grid, solar, battery or tariff (and not the grid meter).
typedef void (*appliance_visit_fn)(const char *graph_id, const char *name, void *ctx);

static bool handle_is_appliance(const char *handle)
{
    if (!handle) return true; // unknown handle: assume appliance (grid is excluded by id)
    if (strncmp(handle, "grid", 4) == 0)    return false;
    if (strncmp(handle, "solar", 5) == 0)   return false;
    if (strncmp(handle, "battery", 7) == 0) return false;
    if (strcmp(handle, "tariff") == 0)      return false; // the tariff source meters nothing
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

// True if the appliance node carries settings.excludeFromScheduling == true.
// Used by the scheduler to drop appliances the user has marked as non-deferrable
// (e.g. a hob or oven). Defaults to false on any missing node/field or parse error.
bool appliance_is_excluded(const char *graph_id)
{
    if (!graph_id) return false;

    char *raw = node_manager_get_all_json();
    if (!raw) return false;
    cJSON *root = cJSON_Parse(raw);
    free(raw);
    if (!root) return false;

    bool excluded = false;
    cJSON *nodes = cJSON_GetObjectItemCaseSensitive(root, "nodes");
    cJSON *n = NULL;
    cJSON_ArrayForEach(n, nodes) {
        const char *nid = json_str(n, "id");
        if (!nid || strcmp(nid, graph_id) != 0) continue;
        cJSON *settings = cJSON_GetObjectItemCaseSensitive(n, "settings");
        cJSON *ex = settings ? cJSON_GetObjectItemCaseSensitive(settings, "excludeFromScheduling") : NULL;
        excluded = cJSON_IsTrue(ex);
        break;
    }

    cJSON_Delete(root);
    return excluded;
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
    snprintf(path, sizeof(path), "%s/profile-%s", SD_BASE, id);
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    // Version 2 profiles end where the per-program stats begin; read them with
    // no programs so the scheduler keeps working until the nightly retrain.
    memset(prof, 0, sizeof(*prof));
    size_t n = fread(prof, 1, sizeof(*prof), f);
    fclose(f);
    if (n < offsetof(appliance_profile_t, program_entries) || prof->magic != APPLIANCE_PROFILE_MAGIC)
        return false;
    if (prof->version == 2) {
        memset((uint8_t *)prof + offsetof(appliance_profile_t, program_entries), 0,
               sizeof(*prof) - offsetof(appliance_profile_t, program_entries));
        return true;
    }
    if (prof->version != APPLIANCE_PROFILE_VERSION || n != sizeof(*prof))
        return false;
    if (prof->program_entries > APPLIANCE_PROFILE_PROGRAMS)
        prof->program_entries = APPLIANCE_PROFILE_PROGRAMS;
    return true;
}

bool appliance_profile_load(const char *graph_id, appliance_profile_t *out)
{
    char id[40];
    if (!out || !sanitize_token(graph_id, id, sizeof(id)))
        return false;
    return load_profile(id, out);
}

// Live state of the appliance's linked device, or nothing if it has none.
static void add_device_json(cJSON *o, const char *id)
{
    appliance_live_t live;
    if (!appliance_state_get(id, &live))
        return;

    cJSON *d = cJSON_AddObjectToObject(o, "device");
    cJSON_AddNumberToObject(d, "node_id",     (double)live.node_id);
    cJSON_AddNumberToObject(d, "endpoint_id", live.endpoint_id);
    cJSON_AddNumberToObject(d, "device_type", live.device_type);
    cJSON_AddBoolToObject(d, "reachable", live.reachable);
    if (live.op_state != APPLIANCE_UNKNOWN) cJSON_AddNumberToObject(d, "operational_state", live.op_state);
    else                                    cJSON_AddNullToObject(d, "operational_state");
    if (live.program_id != APPLIANCE_UNKNOWN) cJSON_AddNumberToObject(d, "program_id", live.program_id);
    else                                      cJSON_AddNullToObject(d, "program_id");
    cJSON_AddStringToObject(d, "program", live.program);
    cJSON_AddStringToObject(d, "phase", live.phase_name);
    if (live.countdown_s >= 0) cJSON_AddNumberToObject(d, "countdown_s", live.countdown_s);
    else                       cJSON_AddNullToObject(d, "countdown_s");
}

// Build a cJSON object for one appliance (always returns an object; "trained"
// is false when no valid profile is on disk).
static cJSON *profile_to_cjson(const char *id)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "graph_id", id);
    add_device_json(o, id);

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

    // Monday first; the profile stores the weekdays by tm_wday (0 = Sunday).
    cJSON *weekdays = cJSON_AddArrayToObject(o, "weekdays");
    for (int i = 0; i < 7; i++) {
        int w = (i + 1) % 7;
        uint16_t days = prof.dow_days[w];
        cJSON *day = cJSON_CreateObject();
        cJSON_AddNumberToObject(day, "days", days);
        cJSON_AddNumberToObject(day, "runs_per_day",
                                days ? (double)prof.dow_runs[w] / days : 0);
        cJSON_AddNumberToObject(day, "energy_wh_per_day",
                                days ? prof.dow_energy_mwh[w] / 1000.0 / days : 0);
        cJSON_AddItemToArray(weekdays, day);
    }

    cJSON *programs = cJSON_AddArrayToObject(o, "programs");
    for (int i = 0; i < prof.program_entries; i++) {
        const appliance_program_profile_t *pp = &prof.programs[i];
        char name[APPLIANCE_LABEL_LEN];
        if (!appliance_state_program_name(id, pp->program_id, name, sizeof(name)))
            snprintf(name, sizeof(name), "Program %u", pp->program_id);

        cJSON *pj = cJSON_CreateObject();
        cJSON_AddNumberToObject(pj, "id",                  pp->program_id);
        cJSON_AddStringToObject(pj, "name",                name);
        cJSON_AddNumberToObject(pj, "runs",                pp->runs);
        cJSON_AddNumberToObject(pj, "avg_program_power_w", pp->avg_power_mw / 1000.0);
        cJSON_AddNumberToObject(pj, "std_program_power_w", pp->std_power_mw / 1000.0);
        cJSON_AddNumberToObject(pj, "avg_program_len_min", pp->avg_len_min);
        cJSON_AddNumberToObject(pj, "std_program_len_min", pp->std_len_min);
        cJSON_AddNumberToObject(pj, "energy_wh",           pp->avg_energy_mwh / 1000.0);
        cJSON *runs = cJSON_AddArrayToObject(pj, "weekday_runs");
        for (int k = 0; k < 7; k++)
            cJSON_AddItemToArray(runs, cJSON_CreateNumber(pp->dow_runs[(k + 1) % 7]));
        cJSON_AddItemToArray(programs, pj);
    }
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
