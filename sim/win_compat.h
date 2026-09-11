/* The shim pulled in only when building the simulator on Windows (MinGW).
 * sim/build.sh passes it with -include.
 *
 * 🚨 Everything here is POSIX and missing from UCRT. Building without knowing
 *    that compiles all 534 files and then dies right before the link — 37
 *    minutes wasted (09-09, on the company PC). */
#pragma once
#include <time.h>

/* 🚨 localtime_s only looks similar: its arguments are the other way round, and it returns 0 on success. */
static inline struct tm *badge_localtime_r(const time_t *t, struct tm *out)
{
    return localtime_s(out, t) == 0 ? out : NULL;
}
#define localtime_r badge_localtime_r

static inline struct tm *badge_gmtime_r(const time_t *t, struct tm *out)
{
    return gmtime_s(out, t) == 0 ? out : NULL;
}
#define gmtime_r badge_gmtime_r
