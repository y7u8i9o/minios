/* M35 test: clocks, calendar conversion, formatting and sleeping.
 * Exits 0 on success. */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <errno.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("timetest: FAIL " __VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    struct timespec mono, real, later;
    CHECK(clock_gettime(CLOCK_MONOTONIC, &mono) == 0, "monotonic clock");
    CHECK(clock_gettime(CLOCK_REALTIME, &real) == 0, "realtime clock");
    CHECK(mono.tv_nsec >= 0 && mono.tv_nsec < 1000000000, "monotonic nanoseconds %ld", (long)mono.tv_nsec);
    long up = uptime_ms();
    long mono_ms = (long)(mono.tv_sec * 1000 + mono.tv_nsec / 1000000);
    CHECK(mono_ms <= up && up - mono_ms < 100, "monotonic %ld ms agrees with uptime %ld ms", mono_ms, up);
    /* The RTC of the virtual machine follows the host: after 2025. */
    CHECK(real.tv_sec > 1735689600, "realtime %ld is after 2025-01-01", (long)real.tv_sec);
    CHECK(real.tv_sec < 4102444800, "realtime %ld is before 2100", (long)real.tv_sec);
    errno = 0;
    CHECK(clock_gettime(7, &later) < 0 && errno == EINVAL, "unknown clock rejected");

    /* Sleeping advances both clocks by about the requested time. */
    struct timespec nap = { 0, 120000000 };
    CHECK(nanosleep(&nap, NULL) == 0, "nanosleep");
    CHECK(clock_gettime(CLOCK_MONOTONIC, &later) == 0, "monotonic after sleep");
    long slept = (long)((later.tv_sec - mono.tv_sec) * 1000 + (later.tv_nsec - mono.tv_nsec) / 1000000);
    CHECK(slept >= 120 && slept < 400, "slept %ld ms", slept);
    CHECK(clock_gettime(CLOCK_REALTIME, &later) == 0, "realtime after sleep");
    long real_slept = (long)((later.tv_sec - real.tv_sec) * 1000 + (later.tv_nsec - real.tv_nsec) / 1000000);
    CHECK(real_slept >= 120 && real_slept < 400, "realtime advanced %ld ms", real_slept);
    time_t now = time(NULL);
    CHECK(now == later.tv_sec || now == later.tv_sec + 1, "time() agrees with the clock");
    struct timeval tv;
    CHECK(gettimeofday(&tv, NULL) == 0 && tv.tv_sec >= now && tv.tv_usec < 1000000, "gettimeofday");

    /* Calendar conversion round trips and known dates. */
    time_t t = 1000000000;                  /* 2001-09-09 01:46:40 UTC, a Sunday */
    struct tm tm;
    CHECK(gmtime_r(&t, &tm) == &tm, "gmtime_r");
    CHECK(tm.tm_year == 101 && tm.tm_mon == 8 && tm.tm_mday == 9 && tm.tm_hour == 1 &&
          tm.tm_min == 46 && tm.tm_sec == 40 && tm.tm_wday == 0 && tm.tm_yday == 251,
          "gmtime of 1000000000: %d-%02d-%02d %02d:%02d:%02d wday %d yday %d",
          tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, tm.tm_wday, tm.tm_yday);
    CHECK(mktime(&tm) == t, "mktime round trip");
    t = 0;
    gmtime_r(&t, &tm);
    CHECK(tm.tm_year == 70 && tm.tm_mon == 0 && tm.tm_mday == 1 && tm.tm_wday == 4, "epoch is Thursday 1970-01-01");
    t = -86400;
    gmtime_r(&t, &tm);
    CHECK(tm.tm_year == 69 && tm.tm_mon == 11 && tm.tm_mday == 31 && tm.tm_hour == 0, "the day before the epoch");
    struct tm leap = { .tm_year = 124, .tm_mon = 1, .tm_mday = 29, .tm_hour = 12 };
    t = mktime(&leap);
    CHECK(t == 1709208000 && leap.tm_wday == 4 && leap.tm_yday == 59, "2024-02-29 12:00 is %ld, wday %d yday %d", (long)t, leap.tm_wday, leap.tm_yday);
    struct tm overflow = { .tm_year = 123, .tm_mon = 11, .tm_mday = 32 };
    t = mktime(&overflow);
    CHECK(overflow.tm_year == 124 && overflow.tm_mon == 0 && overflow.tm_mday == 1, "mktime normalizes December 32nd");
    t = 4102444799;                         /* 2099-12-31 23:59:59 */
    gmtime_r(&t, &tm);
    CHECK(tm.tm_year == 199 && tm.tm_mon == 11 && tm.tm_mday == 31 && tm.tm_hour == 23 && tm.tm_sec == 59, "end of 2099");

    /* Formatting. */
    char buf[128];
    t = 1000000000;
    gmtime_r(&t, &tm);
    CHECK(strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S %a %b %j %p %I %Z", &tm) > 0 &&
          strcmp(buf, "2001-09-09 01:46:40 Sun Sep 252 AM 01 UTC") == 0, "strftime: '%s'", buf);
    CHECK(strftime(buf, sizeof buf, "%F %T %A %B %e %y %C %D %u %w %R %%", &tm) > 0 &&
          strcmp(buf, "2001-09-09 01:46:40 Sunday September  9 01 20 09/09/01 7 0 01:46 %") == 0, "strftime 2: '%s'", buf);
    CHECK(strftime(buf, 8, "%Y-%m-%d", &tm) == 0 && buf[0] == '\0', "strftime overflow");
    CHECK(strcmp(asctime(&tm), "Sun Sep  9 01:46:40 2001\n") == 0, "asctime: '%s'", asctime(&tm));
    CHECK(strcmp(ctime(&t), "Sun Sep  9 01:46:40 2001\n") == 0, "ctime");
    CHECK(difftime(10, 3) == 7.0, "difftime");
    clock_t c0 = clock();
    nanosleep(&nap, NULL);
    CHECK(clock() - c0 >= 120000, "clock advances with wall time");

    /* Setting the clock moves realtime, monotonic stays. */
    struct timespec set = { 1900000000, 0 };   /* 2030-03-17 */
    CHECK(clock_settime(CLOCK_REALTIME, &set) == 0, "clock_settime");
    CHECK(clock_gettime(CLOCK_REALTIME, &later) == 0 && later.tv_sec >= 1900000000 && later.tv_sec < 1900000002, "realtime set: %ld", (long)later.tv_sec);
    CHECK(clock_gettime(CLOCK_MONOTONIC, &later) == 0 && later.tv_sec < 100, "monotonic unaffected");
    CHECK(clock_settime(CLOCK_MONOTONIC, &set) < 0, "monotonic cannot be set");
    CHECK(clock_settime(CLOCK_REALTIME, &real) == 0, "clock restored");

    printf("timetest: %d failures\n", failures);
    return failures ? 1 : 0;
}
