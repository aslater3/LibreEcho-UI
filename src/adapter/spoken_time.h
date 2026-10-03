#ifndef LIBREECHO_SPOKEN_TIME_H
#define LIBREECHO_SPOKEN_TIME_H

#include <stddef.h>
#include <time.h>

/*
 * The clock the device speaks in. Stored as the strings "12" or "24" so the
 * value in the config file and the API reads the way a person would say it.
 */
#define LE_CLOCK_FORMAT_DEFAULT "12"

/* True when `value` is one of the accepted clock formats. */
int le_clock_format_valid(const char *value);

/* 1 for a twelve-hour clock, 0 for twenty-four. Unknown values read as 12. */
int le_clock_format_twelve_hour(const char *value);

/*
 * Writes the local date and time as the device should say it, e.g.
 * "Sunday 30 August, 1:57 PM" or "Sunday 30 August, 13:57".
 */
void le_spoken_time(char *out, size_t size, const struct tm *tm,
                    int twelve_hour);

#endif /* LIBREECHO_SPOKEN_TIME_H */
