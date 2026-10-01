/* Real time clock (M35). The clock is read once at boot, when the
 * monotonic timer has just started, and the difference between the two
 * becomes the epoch offset of CLOCK_REALTIME; the clock is never read
 * again, so a slow device access cannot disturb a running system. The
 * platform reads the device (platform_rtc_read; the CMOS clock on the
 * PC). */
#define KLOG_SUBSYS "rtc"
#include <drivers/rtc.h>
#include <drivers/timer.h>
#include <arch/platform.h>
#include <klog.h>

/* Written by rtc_init and clock_settime, read by clock_gettime; a single
 * aligned word, so no lock. */
static uint64_t epoch_offset_ns;

/* Days from 1970-01-01 to the given date, proleptic Gregorian calendar. */
static int64_t days_from_civil(int64_t y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

int64_t rtc_epoch_seconds(int year, int month, int day, int hour, int minute, int second)
{
    return days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
}

void rtc_init(void)
{
    struct rtc_date a;
    platform_rtc_read(&a);
    int64_t seconds = rtc_epoch_seconds(a.year, a.month, a.day, a.hour, a.minute, a.second);
    if (a.year < 2000 || a.month < 1 || a.month > 12 || a.day < 1 || a.day > 31) {
        klog_warn("implausible clock %04d-%02d-%02d, realtime starts at 2000-01-01", a.year, a.month, a.day);
        seconds = rtc_epoch_seconds(2000, 1, 1, 0, 0, 0);
    }
    epoch_offset_ns = (uint64_t)seconds * 1000000000 - timer_ns();
    klog_info("%04d-%02d-%02d %02d:%02d:%02d UTC, epoch %lld", a.year, a.month, a.day,
              a.hour, a.minute, a.second, (long long)seconds);
}

uint64_t rtc_epoch_offset_ns(void)
{
    return epoch_offset_ns;
}

void rtc_set_epoch_offset_ns(uint64_t ns)
{
    epoch_offset_ns = ns;
}
