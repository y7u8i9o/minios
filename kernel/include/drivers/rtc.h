#pragma once
/* The CMOS real time clock (M35): read once at boot to anchor
 * CLOCK_REALTIME to the monotonic timer. */
#include <kernel.h>

void rtc_init(void);
/* Nanoseconds between the Unix epoch and the timer's zero. */
uint64_t rtc_epoch_offset_ns(void);
void rtc_set_epoch_offset_ns(uint64_t ns);
/* Seconds since the Unix epoch for a UTC calendar date. */
int64_t rtc_epoch_seconds(int year, int month, int day, int hour, int minute, int second);
