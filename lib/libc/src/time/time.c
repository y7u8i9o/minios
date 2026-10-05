/* Clocks and sleeping (M35). */
#include <time.h>
#include <sys/time.h>
#include <minios/syscall.h>
#include <unistd.h>
#include <errno.h>

int clock_gettime(clockid_t clock, struct timespec *ts)
{
    return (int)syscall2(SYS_clock_gettime, clock, ts);
}

int clock_settime(clockid_t clock, const struct timespec *ts)
{
    return (int)syscall2(SYS_clock_settime, clock, ts);
}

int clock_getres(clockid_t clock, struct timespec *res)
{
    if (clock != CLOCK_REALTIME && clock != CLOCK_MONOTONIC) {
        errno = EINVAL;
        return -1;
    }
    if (res) {
        res->tv_sec = 0;
        res->tv_nsec = 1000;        /* the TSC is read at nanosecond scale; the timer ticks every ms */
    }
    return 0;
}

time_t time(time_t *out)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) < 0)
        return (time_t)-1;
    if (out)
        *out = (time_t)ts.tv_sec;
    return (time_t)ts.tv_sec;
}

/* Processor time is not accounted per process: the monotonic clock
 * stands in, which makes clock() differences wall time. */
clock_t clock(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return (clock_t)-1;
    return (clock_t)(ts.tv_sec * CLOCKS_PER_SEC + ts.tv_nsec / 1000);
}

double difftime(time_t a, time_t b)
{
    return (double)a - (double)b;
}

int nanosleep(const struct timespec *request, struct timespec *remain)
{
    if (!request || request->tv_nsec < 0 || request->tv_nsec >= 1000000000 || request->tv_sec < 0) {
        errno = EINVAL;
        return -1;
    }
    unsigned long ms = (unsigned long)request->tv_sec * 1000 + (unsigned long)((request->tv_nsec + 999999) / 1000000);
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    if (ms && sleep_ms(ms) < 0) {
        /* A signal ended the sleep (EINTR).  remain receives the requested
         * time minus the time slept, as POSIX requires. */
        if (remain) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long long left = (long long)request->tv_sec * 1000000000LL + request->tv_nsec -
                             ((long long)(now.tv_sec - start.tv_sec) * 1000000000LL + (now.tv_nsec - start.tv_nsec));
            if (left < 0)
                left = 0;
            remain->tv_sec = (time_t)(left / 1000000000LL);
            remain->tv_nsec = (long)(left % 1000000000LL);
        }
        return -1;
    }
    if (remain) {
        remain->tv_sec = 0;
        remain->tv_nsec = 0;
    }
    return 0;
}

int timespec_get(struct timespec *ts, int base)
{
    if (base != TIME_UTC || !ts)
        return 0;
    return clock_gettime(CLOCK_REALTIME, ts) == 0 ? base : 0;
}

int timespec_getres(struct timespec *res, int base)
{
    if (base != TIME_UTC || !res)
        return 0;
    return clock_getres(CLOCK_REALTIME, res) == 0 ? base : 0;
}

int gettimeofday(struct timeval *tv, struct timezone *tz)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) < 0)
        return -1;
    if (tv) {
        tv->tv_sec = (time_t)ts.tv_sec;
        tv->tv_usec = (suseconds_t)(ts.tv_nsec / 1000);
    }
    if (tz) {
        tz->tz_minuteswest = 0;
        tz->tz_dsttime = 0;
    }
    return 0;
}

int settimeofday(const struct timeval *tv, const struct timezone *tz)
{
    if (!tv) {
        errno = EINVAL;
        return -1;
    }
    struct timespec ts = { tv->tv_sec, tv->tv_usec * 1000 };
    return clock_settime(CLOCK_REALTIME, &ts);
}

int adjtime(const struct timeval *delta, struct timeval *olddelta)
{
    return (int)syscall2(SYS_adjtime, delta, olddelta);
}
