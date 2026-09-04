/*
 * Spoken date and time.
 *
 * agentd states the current time inside the turn prompt and the model reads it
 * straight back, so the format here is what the device actually says out loud.
 * A 24-hour clock is correct for some households and wrong for others, which is
 * why it became a setting; these cases pin the parts that are easy to get
 * wrong when switching between them.
 */

#include "adapter/spoken_time.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static struct tm at(int hour, int minute)
{
    struct tm tm;

    memset(&tm, 0, sizeof(tm));
    tm.tm_year = 126;          /* 2026 */
    tm.tm_mon = 7;             /* August */
    tm.tm_mday = 30;
    tm.tm_wday = 0;            /* Sunday */
    tm.tm_hour = hour;
    tm.tm_min = minute;
    return tm;
}

int main(void)
{
    char out[64];
    struct tm tm;

    /* 24-hour keeps the zero-padded form it has always used. */
    tm = at(13, 57);
    le_spoken_time(out, sizeof(out), &tm, 0);
    assert(strstr(out, "13:57") != NULL);
    assert(strstr(out, "PM") == NULL);
    assert(strstr(out, "August") != NULL);   /* the date survives either way */

    tm = at(9, 5);
    le_spoken_time(out, sizeof(out), &tm, 0);
    assert(strstr(out, "09:05") != NULL);

    /* 12-hour drops the leading zero: spoken aloud, "01:57 PM" is wrong. */
    tm = at(13, 57);
    le_spoken_time(out, sizeof(out), &tm, 1);
    assert(strstr(out, "1:57 PM") != NULL);
    assert(strstr(out, "01:57") == NULL);
    assert(strstr(out, "13:57") == NULL);

    tm = at(9, 5);
    le_spoken_time(out, sizeof(out), &tm, 1);
    assert(strstr(out, "9:05 AM") != NULL);
    assert(strstr(out, "09:05") == NULL);

    /*
     * Midnight and noon are where a naive hour%12 gives "0:00". Both must read
     * as 12, and the meridiem must not flip.
     */
    tm = at(0, 5);
    le_spoken_time(out, sizeof(out), &tm, 1);
    assert(strstr(out, "12:05 AM") != NULL);

    tm = at(12, 0);
    le_spoken_time(out, sizeof(out), &tm, 1);
    assert(strstr(out, "12:00 PM") != NULL);

    tm = at(23, 59);
    le_spoken_time(out, sizeof(out), &tm, 1);
    assert(strstr(out, "11:59 PM") != NULL);

    tm = at(12, 30);
    le_spoken_time(out, sizeof(out), &tm, 1);
    assert(strstr(out, "12:30 PM") != NULL);

    /* Only "12" and "24" are accepted; anything else leaves the caller's value. */
    assert(le_clock_format_twelve_hour("12") == 1);
    assert(le_clock_format_twelve_hour("24") == 0);
    assert(le_clock_format_valid("12"));
    assert(le_clock_format_valid("24"));
    assert(!le_clock_format_valid("13"));
    assert(!le_clock_format_valid(""));
    assert(!le_clock_format_valid(NULL));
    assert(!le_clock_format_valid("12h"));
    assert(!le_clock_format_valid("twelve"));

    /* A buffer too small must truncate rather than overrun. */
    {
        char tiny[8];

        tm = at(13, 57);
        le_spoken_time(tiny, sizeof(tiny), &tm, 1);
        assert(strlen(tiny) < sizeof(tiny));
    }

    puts("spoken_time: 12/24-hour formatting, midnight, noon and validation ok");
    return 0;
}
