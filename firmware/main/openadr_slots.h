#pragma once

#include <stddef.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// A local day has 23, 24 or 25 hour slots depending on DST.
#define OPENADR_MAX_DAY_SLOTS 25

// Pure time arithmetic for OpenADR reports: no ESP-IDF dependencies, so it is
// also built and tested on the host (firmware/host_test). Uses the process TZ.

// Fill `starts` with the UTC start of every hour slot in the local day
// `date_str` (YYYY-MM-DD), from local midnight up to the next local midnight.
// starts[0] is local midnight. Returns the slot count (23, 24 or 25), or -1 if
// the date is malformed or `max` is too small.
int openadr_day_slots(const char *date_str, time_t *starts, int max);

// Local hour-of-day (0-23) of a slot. The stored forecasts hold one record per
// local hour, indexed by hour, so this is the record a slot reads: on the
// 25-hour day the repeated 01:00 reads record 1 twice, and on the 23-hour day
// record 1 is unused.
int openadr_slot_local_hour(time_t slot_start);

// Format as RFC 3339 UTC, "YYYY-MM-DDTHH:MM:SSZ". `len` must be at least 21.
void openadr_format_utc(time_t t, char *buf, size_t len);

#ifdef __cplusplus
}
#endif
