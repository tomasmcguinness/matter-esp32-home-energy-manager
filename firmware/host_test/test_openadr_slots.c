// Host-side test for the OpenADR report slot arithmetic across DST changes.
// Build and run with ./run.sh (plain gcc, no ESP-IDF needed).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "openadr_slots.h"

static int s_failures = 0;

#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            s_failures++;                                 \
            printf("  FAIL: " __VA_ARGS__);               \
            printf("\n");                                 \
        }                                                 \
    } while (0)

static void check_day(const char *date, int want_count, const char *want_first,
                      const char *want_last, const int *want_hours)
{
    time_t starts[OPENADR_MAX_DAY_SLOTS];
    int count = openadr_day_slots(date, starts, OPENADR_MAX_DAY_SLOTS);

    printf("%s: %d slots\n", date, count);
    CHECK(count == want_count, "expected %d slots, got %d", want_count, count);
    if (count <= 0)
        return;

    char first[24], last[24];
    openadr_format_utc(starts[0], first, sizeof(first));
    openadr_format_utc(starts[count - 1], last, sizeof(last));
    printf("  first %s, last %s\n", first, last);
    CHECK(strcmp(first, want_first) == 0, "first slot: expected %s, got %s", want_first, first);
    CHECK(strcmp(last, want_last) == 0, "last slot: expected %s, got %s", want_last, last);

    for (int i = 0; i < count; i++) {
        if (i > 0)
            CHECK(starts[i] - starts[i - 1] == 3600, "slot %d is not one hour after slot %d", i, i - 1);
        int hour = openadr_slot_local_hour(starts[i]);
        CHECK(hour == want_hours[i], "slot %d: expected local hour %d, got %d", i, want_hours[i], hour);
    }
}

int main(void)
{
    // Same zone as the firmware sets in main.cpp.
    setenv("TZ", "GMT0BST,M3.5.0/1,M10.5.0", 1);
    tzset();

    // Winter day (GMT): local midnight is UTC midnight.
    static const int normal[24] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23};
    check_day("2026-01-15", 24, "2026-01-15T00:00:00Z", "2026-01-15T23:00:00Z", normal);

    // Summer day (BST): local midnight is 23:00 UTC the day before.
    check_day("2026-07-01", 24, "2026-06-30T23:00:00Z", "2026-07-01T22:00:00Z", normal);

    // Clocks go forward: 01:00 GMT becomes 02:00 BST, so local hour 1 never happens.
    static const int spring[23] = {0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23};
    check_day("2026-03-29", 23, "2026-03-29T00:00:00Z", "2026-03-29T22:00:00Z", spring);

    // Clocks go back: 02:00 BST becomes 01:00 GMT, so local hour 1 happens twice.
    static const int autumn[25] = {0, 1, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23};
    check_day("2026-10-25", 25, "2026-10-24T23:00:00Z", "2026-10-25T23:00:00Z", autumn);

    time_t starts[OPENADR_MAX_DAY_SLOTS];
    CHECK(openadr_day_slots("not-a-date", starts, OPENADR_MAX_DAY_SLOTS) == -1, "malformed date accepted");
    CHECK(openadr_day_slots("2026-10-25", starts, 24) == -1, "25-slot day fitted into 24");

    printf(s_failures ? "\n%d check(s) FAILED\n" : "\nAll checks passed\n", s_failures);
    return s_failures ? 1 : 0;
}
