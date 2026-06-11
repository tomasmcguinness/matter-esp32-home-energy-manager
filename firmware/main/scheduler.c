#include "scheduler.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "power_logger.h"      // power_record_t
#include "appliance_profile.h" // appliance_enumerate, appliance_profile_load

#define SD_BASE     "/sdcard"
#define SCHED_HOURS  24

// Copy a date token only if filesystem-safe ([A-Za-z0-9_-], length 1..15);
// "YYYY-MM-DD" passes. Guards the path we build under /sdcard.
static bool sanitize_date(const char *tok, char *out, size_t out_len)
{
    if (!tok) return false;
    size_t n = strlen(tok);
    if (n == 0 || n >= out_len || n > 15) return false;
    for (size_t i = 0; i < n; i++) {
        char c = tok[i];
        bool ok = (c >= '0' && c <= '9') || c == '-';
        if (!ok) return false;
        out[i] = c;
    }
    out[n] = '\0';
    return true;
}

// Load the day's hourly surplus (watts) into surplus_w[24]; hours past the end
// of the file are left at 0. Returns false if the file is missing/empty.
static bool load_surplus(const char *date, float surplus_w[SCHED_HOURS])
{
    char path[64];
    snprintf(path, sizeof(path), "%s/surplus-%s", SD_BASE, date);
    FILE *f = fopen(path, "rb");
    if (!f) return false;

    for (int i = 0; i < SCHED_HOURS; i++) surplus_w[i] = 0.0f;

    power_record_t rec;
    int n = 0;
    while (n < SCHED_HOURS && fread(&rec, sizeof(rec), 1, f) == 1) {
        surplus_w[n++] = rec.power_mw / 1000.0f;
    }
    fclose(f);
    return n > 0;
}

int scheduler_compute(const char *date_str, scheduled_run_t *out, int max)
{
    char date[16];
    if (!sanitize_date(date_str, date, sizeof(date))) return -1;

    float surplus_w[SCHED_HOURS];
    if (!load_surplus(date, surplus_w)) return -1;

    // Only positive surplus is available to soak up (negative = grid import).
    float remaining[SCHED_HOURS];
    for (int i = 0; i < SCHED_HOURS; i++)
        remaining[i] = surplus_w[i] > 0.0f ? surplus_w[i] : 0.0f;

    char ids[SCHEDULER_MAX_RUNS][APPLIANCE_ID_MAX_LEN];
    char names[SCHEDULER_MAX_RUNS][APPLIANCE_NAME_MAX_LEN];
    size_t na = appliance_enumerate(ids, names, SCHEDULER_MAX_RUNS);

    int count = 0;
    for (size_t a = 0; a < na && count < max; a++) {
        scheduled_run_t *r = &out[count];
        memset(r, 0, sizeof(*r));
        snprintf(r->graph_id, sizeof(r->graph_id), "%s", ids[a]);
        snprintf(r->name, sizeof(r->name), "%s", names[a]);
        r->start_hour = -1;

        appliance_profile_t prof;
        if (appliance_profile_load(ids[a], &prof) &&
            prof.program_count > 0 && prof.avg_program_power_mw > 0) {
            int dur = (int)((prof.avg_program_len_min + 59) / 60); // ceil to hours
            if (dur < 1) dur = 1;
            if (dur > SCHED_HOURS) dur = SCHED_HOURS;
            r->duration_hours = dur;
            r->avg_power_w    = prof.avg_program_power_mw / 1000.0f;
            r->energy_wh      = r->avg_power_w * dur;
        }
        count++;
    }

    // Place schedulable appliances largest-energy first so the biggest loads get
    // first claim on the surplus; ties keep enumeration order.
    int idx[SCHEDULER_MAX_RUNS];
    int m = 0;
    for (int i = 0; i < count; i++)
        if (out[i].duration_hours > 0) idx[m++] = i;
    for (int i = 0; i < m; i++)
        for (int j = i + 1; j < m; j++)
            if (out[idx[j]].energy_wh > out[idx[i]].energy_wh) {
                int t = idx[i]; idx[i] = idx[j]; idx[j] = t;
            }

    for (int k = 0; k < m; k++) {
        scheduled_run_t *r = &out[idx[k]];
        int   dur   = r->duration_hours;
        float power = r->avg_power_w;

        // Slide the run over every valid start hour, scoring self-consumption.
        float best_score = 0.0f;
        int   best_s     = -1;
        for (int s = 0; s + dur <= SCHED_HOURS; s++) {
            float score = 0.0f;
            for (int i = 0; i < dur; i++) {
                float use = power < remaining[s + i] ? power : remaining[s + i];
                score += use;
            }
            if (score > best_score) { best_score = score; best_s = s; }
        }

        // best_s stays -1 when no window soaks any surplus: leave it unscheduled
        // rather than suggest a pointless run.
        if (best_s >= 0) {
            r->start_hour          = best_s;
            r->self_consumption_wh = best_score;
            for (int i = 0; i < dur; i++) {
                float use = power < remaining[best_s + i] ? power : remaining[best_s + i];
                remaining[best_s + i] -= use;
            }
        }
    }

    return count;
}

char *scheduler_json(const char *date_str)
{
    scheduled_run_t runs[SCHEDULER_MAX_RUNS];
    int n = scheduler_compute(date_str, runs, SCHEDULER_MAX_RUNS);

    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddStringToObject(root, "date", date_str ? date_str : "");
    cJSON_AddBoolToObject(root, "surplus_available", n >= 0);
    cJSON *arr = cJSON_AddArrayToObject(root, "runs");

    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "graph_id", runs[i].graph_id);
        cJSON_AddStringToObject(o, "name", runs[i].name);
        cJSON_AddBoolToObject(o, "schedulable", runs[i].duration_hours > 0);
        cJSON_AddNumberToObject(o, "start_hour", runs[i].start_hour);
        cJSON_AddNumberToObject(o, "duration_hours", runs[i].duration_hours);
        cJSON_AddNumberToObject(o, "avg_power_w", runs[i].avg_power_w);
        cJSON_AddNumberToObject(o, "energy_wh", runs[i].energy_wh);
        cJSON_AddNumberToObject(o, "self_consumption_wh", runs[i].self_consumption_wh);
        cJSON_AddItemToArray(arr, o);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json; // caller must free
}
