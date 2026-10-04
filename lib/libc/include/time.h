#pragma once
/* Time (M35, L2): clocks from the kernel, calendar conversion in UTC and
 * in the local time zone, formatting and sleeping. */
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

/* Calendar conversion. gmtime and timegm use UTC. localtime and mktime
 * use the zone of TZ, or of /etc/localtime when TZ is not set, or UTC
 * (docs/design/time.md). */
struct tm *gmtime(const time_t *t);
struct tm *gmtime_r(const time_t *t, struct tm *out);
struct tm *localtime(const time_t *t);
struct tm *localtime_r(const time_t *t, struct tm *out);
time_t timegm(struct tm *tm);
time_t mktime(struct tm *tm);

/* Formatting: strftime supports %a %A %b %B %c %C %d %D %e %F %g %G %h
 * %H %I %j %k %l %m %M %n %p %P %r %R %s %S %t %T %u %U %V %w %W %x %X %y
 * %Y %z %Z %% and the modifiers E and O. Names and the formats of %c, %x,
 * %X and %r come from LC_TIME, and %OB and %Ob give the month names used
 * without a day. */
size_t strftime(char *buf, size_t size, const char *format, const struct tm *tm);
struct __locale_struct;
size_t strftime_l(char *buf, size_t size, const char *format, const struct tm *tm, struct __locale_struct *loc);
char *asctime(const struct tm *tm);
char *asctime_r(const struct tm *tm, char *buf);   /* at least 26 bytes */
char *ctime(const time_t *t);
char *ctime_r(const time_t *t, char *buf);

/* The names and the standard offset in seconds west of UTC of the local
 * zone, set by tzset, localtime and mktime. */
extern char *tzname[2];
extern long timezone;
extern int daylight;
void tzset(void);
