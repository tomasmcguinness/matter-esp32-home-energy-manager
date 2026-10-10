#include "openadr_slots.h"

#include <stdio.h>

// Local midnight at the start of y-m-d. tm_isdst = -1 lets mktime work out
// whether DST applies; mktime also normalises an out-of-range day.
static time_t local_midnight(int y, int m, int d)
{
    struct tm tm = {0};
    tm.tm_year  = y - 1900;
    tm.tm_mon   = m - 1;
    tm.tm_mday  = d;
    tm.tm_isdst = -1;
    return mktime(&tm);
}

int openadr_day_slots(const char *date_str, time_t *starts, int max)
{
    int y, m, d;
    if (!date_str || sscanf(date_str, "%d-%d-%d", &y, &m, &d) != 3)
        return -1;

    time_t start = local_midnight(y, m, d);
    time_t end   = local_midnight(y, m, d + 1);
    if (start == (time_t)-1 || end <= start)
        return -1;

    int count = (int)((end - start) / 3600);
    if (count > max)
        return -1;

    for (int i = 0; i < count; i++)
        starts[i] = start + (time_t)i * 3600;
    return count;
}

int openadr_slot_local_hour(time_t slot_start)
{
    struct tm tm;
    localtime_r(&slot_start, &tm);
    return tm.tm_hour;
}

void openadr_format_utc(time_t t, char *buf, size_t len)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm);
}
