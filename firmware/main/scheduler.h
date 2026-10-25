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
    char  program[32];          // program whose profile was used; "" for all cycles
    const char *program_source; // "selected" | "weekday" | "all"
} scheduled_run_t;

// Build a suggested appliance schedule for date_str ("YYYY-MM-DD") by sliding
// each appliance's learned run across that day's hourly surplus forecast
// (/sdcard/surplus-<date>), greedily placing the largest loads first and
// subtracting each placement from the remaining surplus. Fills out[] (up to
// max) and returns the count, or -1 if the surplus forecast for that day is
// missing.
//
// An appliance linked to a Matter appliance device is sized from one program's
// profile: the program selected on the machine when date_str is today and it
// isn't already running, otherwise the program run most on that weekday. With
// neither (or no per-program history) the profile across all cycles is used.
int scheduler_compute(const char *date_str, scheduled_run_t *out, int max);

// Same computation rendered as JSON for the web API. Caller must free.
// Shape: {"date":"..","surplus_available":bool,"runs":[{"graph_id","name",
//   "schedulable":bool,"start_hour","duration_hours","avg_power_w",
//   "energy_wh","self_consumption_wh","program","program_source"},...]}
char *scheduler_json(const char *date_str);

#ifdef __cplusplus
}
#endif
