#pragma once
/* Time (M35): clocks from the kernel, calendar conversion in UTC (the
 * system has no time zones), formatting and sleeping. */
#include <stddef.h>
#include <sys/types.h>
#include <minios/abi.h>

#define CLOCKS_PER_SEC 1000000L
#define TIME_UTC 1

typedef long clock_t;
typedef int clockid_t;

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;                /* 1 to 31 */
    int tm_mon;                 /* 0 to 11 */
    int tm_year;                /* years since 1900 */
    int tm_wday;                /* days since Sunday */
    int tm_yday;                /* days since January 1 */
    int tm_isdst;
};

/* Clocks. */
int clock_gettime(clockid_t clock, struct timespec *ts);
int clock_settime(clockid_t clock, const struct timespec *ts);
int clock_getres(clockid_t clock, struct timespec *res);
time_t time(time_t *out);
clock_t clock(void);
double difftime(time_t a, time_t b);
/* Sleeps in whole milliseconds, rounded up; the remainder is always 0. */
int nanosleep(const struct timespec *request, struct timespec *remain);
int timespec_get(struct timespec *ts, int base);
int timespec_getres(struct timespec *res, int base);

/* Calendar conversion, UTC only. localtime is gmtime. */
struct tm *gmtime(const time_t *t);
struct tm *gmtime_r(const time_t *t, struct tm *out);
struct tm *localtime(const time_t *t);
struct tm *localtime_r(const time_t *t, struct tm *out);
time_t timegm(struct tm *tm);
time_t mktime(struct tm *tm);

/* Formatting: strftime supports %a %A %b %B %c %C %d %D %e %F %H %I %j
 * %m %M %n %p %R %S %t %T %u %w %x %X %y %Y %z %Z %%. */
size_t strftime(char *buf, size_t size, const char *format, const struct tm *tm);
char *asctime(const struct tm *tm);
char *asctime_r(const struct tm *tm, char *buf);   /* at least 26 bytes */
char *ctime(const time_t *t);
char *ctime_r(const time_t *t, char *buf);

/* MiniOS has no time-zone database: the sole zone is UTC. */
extern char *tzname[2];
extern long timezone;
extern int daylight;
void tzset(void);
