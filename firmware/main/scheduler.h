#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Most we'll ever schedule in one pass (only a handful of appliance slots exist;
// this also bounds the on-stack working set in scheduler_compute/_json).
#define SCHEDULER_MAX_RUNS 16

// One appliance's suggested run for a day. start_hour is -1 when the appliance
// has no learned program yet, or when there is no surplus to soak up.
typedef struct {
    char  graph_id[40];
    char  name[64];
    int   start_hour;           // 0..23 local start hour; -1 if not placed
    int   duration_hours;       // ceil(avg_program_len_min/60), 1..24; 0 if no profile
    float avg_power_w;          // mean draw while running
    float energy_wh;            // avg_power_w * duration_hours
    float self_consumption_wh;  // Σ min(power, surplus) over the chosen window
} scheduled_run_t;

// Build a suggested appliance schedule for date_str ("YYYY-MM-DD") by sliding
// each appliance's learned run across that day's hourly surplus forecast
// (/sdcard/surplus-<date>), greedily placing the largest loads first and
// subtracting each placement from the remaining surplus. Fills out[] (up to
// max) and returns the count, or -1 if the surplus forecast for that day is
// missing.
int scheduler_compute(const char *date_str, scheduled_run_t *out, int max);

// Same computation rendered as JSON for the web API. Caller must free.
// Shape: {"date":"..","surplus_available":bool,"runs":[{"graph_id","name",
//   "schedulable":bool,"start_hour","duration_hours","avg_power_w",
//   "energy_wh","self_consumption_wh"},...]}
char *scheduler_json(const char *date_str);

#ifdef __cplusplus
}
#endif
