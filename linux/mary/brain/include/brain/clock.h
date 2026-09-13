/* The wall clock the voice instructions start with (PromptCatalog+Voice.swift's
 * sewnPreamble formats it through DateFormatter). MaryOS speaks English, so day
 * and month names are English. */
#ifndef MARY_BRAIN_CLOCK_H
#define MARY_BRAIN_CLOCK_H

#include <stddef.h>

typedef struct mb_clock {
    int year;
    int month;          /* 1…12 */
    int day;            /* 1…31 */
    int weekday;        /* 0 Sunday … 6 Saturday */
    int hour;           /* 0…23 */
    int minute;
    char zone[64];      /* an IANA name: "America/Los_Angeles" */
} mb_clock;

/* Now, in local time, with the zone from $TZ or /etc/localtime ("UTC" when neither says). 0, or -errno. */
int mb_clock_now(mb_clock *out);
/* formatter("h:mm a"): "7:05 PM". */
void mb_clock_time(const mb_clock *clock, char *out, size_t cap);
/* formatter("EEEE, MMMM d, yyyy"): "Saturday, September 12, 2026". */
void mb_clock_date(const mb_clock *clock, char *out, size_t cap);

#endif
