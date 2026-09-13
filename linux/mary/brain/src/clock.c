#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#include "brain/clock.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char *const WEEKDAYS[7] = { "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday" };
static const char *const MONTHS[12] = { "January", "February", "March", "April", "May", "June", "July",
                                        "August", "September", "October", "November", "December" };

int mb_clock_now(mb_clock *out) {
    time_t now = time(NULL);
    struct tm tm;
    if (!localtime_r(&now, &tm)) return -errno;
    out->year = tm.tm_year + 1900;
    out->month = tm.tm_mon + 1;
    out->day = tm.tm_mday;
    out->weekday = tm.tm_wday;
    out->hour = tm.tm_hour;
    out->minute = tm.tm_min;
    snprintf(out->zone, sizeof out->zone, "UTC");
    const char *tz = getenv("TZ");
    if (tz && *tz) {
        snprintf(out->zone, sizeof out->zone, "%s", *tz == ':' ? tz + 1 : tz);
    } else {
        char link[256];
        ssize_t n = readlink("/etc/localtime", link, sizeof link - 1);
        if (n > 0) {
            link[n] = 0;
            char *zone = strstr(link, "zoneinfo/");
            if (zone && zone[9]) snprintf(out->zone, sizeof out->zone, "%s", zone + 9);
        }
    }
    return 0;
}

void mb_clock_time(const mb_clock *c, char *out, size_t cap) {
    int h = c->hour % 12;
    snprintf(out, cap, "%d:%02d %s", h ? h : 12, c->minute, c->hour < 12 ? "AM" : "PM");
}

void mb_clock_date(const mb_clock *c, char *out, size_t cap) {
    snprintf(out, cap, "%s, %s %d, %d", WEEKDAYS[((c->weekday % 7) + 7) % 7], MONTHS[((c->month - 1) % 12 + 12) % 12], c->day, c->year);
}
