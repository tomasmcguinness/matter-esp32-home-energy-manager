#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define APPLIANCE_PROFILE_MAGIC   0x41505231u  // "APR1"
#define APPLIANCE_PROFILE_VERSION 1

// Learned per-appliance usage profile, derived nightly from a rolling window of
// the appliance's per-minute power history (/littlefs/node-<graphId>-*). The
// scheduler uses these to slide an appliance's run across the predicted surplus.
//
// "standby" is the idle draw when the appliance is plugged in but not running; a
// "program" (run cycle) is a contiguous stretch where the draw rises above
// standby. Program stats are the mean and stddev across all cycles in the window.
// Persisted to /littlefs/profile-<graphId>.
typedef struct {
    uint32_t magic;                // APPLIANCE_PROFILE_MAGIC
    uint16_t version;              // APPLIANCE_PROFILE_VERSION
    uint16_t window_days;          // days of history scanned
    uint32_t trained_unix;         // when the profile was computed
    int32_t  standby_mw;           // estimated idle draw
    int32_t  avg_program_power_mw; // mean power while running
    int32_t  std_program_power_mw; // stddev of per-cycle mean power
    uint32_t avg_program_len_min;  // mean cycle length (minutes)
    uint32_t std_program_len_min;  // stddev of cycle length (minutes)
    uint16_t program_count;        // cycles detected in the window
    uint16_t days_with_data;       // days that had any minute records
} appliance_profile_t;

// Analyse one appliance's last window_days of per-minute history, derive its
// standby power, average program power and average program length, and persist
// the profile to /littlefs/profile-<graph_id>. Returns the number of program
// cycles detected (0 if the appliance never ran in the window — a profile is
// still written with the standby figure and zeroed program fields).
int appliance_profile_train(const char *graph_id, int window_days);

// Enumerate every appliance node in the topology graph (any node wired to the
// consumer unit that is not the grid, solar or battery) and train each.
void appliance_profile_train_all(int window_days);

// Buffer sizes for appliance_enumerate(): graph ids are filesystem-safe tokens
// (<=32 chars); names come from the node's settings label and may carry emoji.
#define APPLIANCE_ID_MAX_LEN   40
#define APPLIANCE_NAME_MAX_LEN 64

// List the appliance nodes in the topology graph (same set trained nightly).
// Fills ids[i] with the stable graph id and names[i] with the user-facing name
// (settings.name, falling back to settings.label, then the id). Returns the
// number written (capped at max). Used by the scheduler.
size_t appliance_enumerate(char ids[][APPLIANCE_ID_MAX_LEN],
                           char names[][APPLIANCE_NAME_MAX_LEN],
                           size_t max);

// Load one appliance's persisted profile from /littlefs/profile-<graph_id> into
// out, validating the magic/version. Returns false if absent or invalid.
bool appliance_profile_load(const char *graph_id, appliance_profile_t *out);

// Read back one appliance's persisted profile as JSON. Caller must free.
// Shape: {"trained":bool,"graph_id":"..","standby_w":F,"avg_program_power_w":F,
//   "std_program_power_w":F,"avg_program_len_min":N,"std_program_len_min":N,
//   "program_count":N,"days_with_data":N,"trained_unix":N,"window_days":N}
char *appliance_profile_json(const char *graph_id);

// List every appliance's persisted profile as a JSON array. Caller must free.
// Shape: {"appliances":[ <object as above>, ... ]}
char *appliance_profile_all_json(void);

#ifdef __cplusplus
}
#endif
