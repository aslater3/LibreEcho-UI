#include "spoken_time.h"

#include <stdio.h>
#include <string.h>

int le_clock_format_valid(const char *value)
{
    return value && (!strcmp(value, "12") || !strcmp(value, "24"));
}

int le_clock_format_twelve_hour(const char *value)
{
    return !(value && !strcmp(value, "24"));
}

void le_spoken_time(char *out, size_t size, const struct tm *tm,
                    int twelve_hour)
{
    char date[48] = "";

    if (!out || size == 0)
        return;
    out[0] = '\0';
    if (!tm)
        return;
    (void)strftime(date, sizeof(date), "%A %e %B", tm);
    if (!twelve_hour) {
        (void)snprintf(out, size, "%s, %02d:%02d",
                       date, tm->tm_hour, tm->tm_min);
        return;
    }
    /*
     * strftime's %I would zero-pad, and "01:57 PM" read aloud is wrong, so the
     * hour is formatted by hand. Midnight and noon are the cases a plain
     * hour % 12 gets wrong: both must say twelve, not zero.
     */
    {
        int hour = tm->tm_hour % 12;

        if (hour == 0)
            hour = 12;
        (void)snprintf(out, size, "%s, %d:%02d %s", date, hour, tm->tm_min,
                       tm->tm_hour < 12 ? "AM" : "PM");
    }
}
