#pragma once
/* The real time clock (M35): read once at boot to anchor CLOCK_REALTIME
 * to the monotonic timer. The platform reads the device. */
#include <kernel.h>

/* A UTC calendar date as read from the clock device. */
struct rtc_date {
    int year, month, day, hour, minute, second;
};

void rtc_init(void);
/* The realtime clock in nanoseconds since the Unix epoch, with the part of
 * a slew that is due. */
uint64_t rtc_realtime_ns(void);
/* Step the realtime clock to ns. A slew in progress ends. */
void rtc_set_realtime_ns(uint64_t ns);
/* Slew the realtime clock by *delta nanoseconds at 500 microseconds per
 * second, in place of the slew in progress, or only report it when delta
 * is NULL. Returns the correction that remained of the earlier slew. */
int64_t rtc_adjust(const int64_t *delta);
/* Seconds since the Unix epoch for a UTC calendar date. */
int64_t rtc_epoch_seconds(int year, int month, int day, int hour, int minute, int second);
