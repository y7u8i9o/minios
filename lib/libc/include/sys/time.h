#pragma once
#include <time.h>

typedef long suseconds_t;

struct timeval {
    time_t tv_sec;
    suseconds_t tv_usec;
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

int gettimeofday(struct timeval *tv, struct timezone *tz);
int settimeofday(const struct timeval *tv, const struct timezone *tz);
/* Slew the realtime clock by delta at 500 microseconds per second, in place
 * of the slew in progress, and store the correction that remained in
 * olddelta. Either may be NULL. Changing the clock requires root (BSD). */
int adjtime(const struct timeval *delta, struct timeval *olddelta);
