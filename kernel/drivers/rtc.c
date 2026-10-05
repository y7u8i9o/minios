/* Real time clock (M35). The clock is read once at boot, when the
 * monotonic timer has just started, and the difference between the two
 * becomes the epoch offset of CLOCK_REALTIME; the clock is never read
 * again, so a slow device access cannot disturb a running system. The
 * platform reads the device (platform_rtc_read; the CMOS clock on the
 * PC). clock_settime steps the clock and adjtime slews it (V2). */
#define KLOG_SUBSYS "rtc"
#include <drivers/rtc.h>
#include <lib/date.h>
#include <drivers/timer.h>
#include <arch/platform.h>
#include <klog.h>
#include <sync/spinlock.h>

/* The realtime clock is the monotonic timer plus epoch_offset_ns. adjtime
 * corrects the offset gradually: slew_ns is the correction that remains,
 * and every read applies one nanosecond of it per SLEW_RATE nanoseconds of
 * the timer since slew_at, 500 microseconds per second. A correction of
 * the offset is smaller than the time that passed, so the realtime clock
 * never runs backwards during a slew. clock_lock protects epoch_offset_ns,
 * slew_ns and slew_at. */
#define SLEW_RATE 2000
static DEFINE_SPINLOCK(clock_lock);
static uint64_t epoch_offset_ns;
static int64_t slew_ns;
static uint64_t slew_at;

/* Applies the part of the slew that is due at now. */
static void slew_locked(uint64_t now)
{
    if (slew_ns != 0 && now > slew_at) {
        uint64_t step = (now - slew_at) / SLEW_RATE;
        uint64_t left = slew_ns < 0 ? (uint64_t)-slew_ns : (uint64_t)slew_ns;
        if (step > left)
            step = left;
        if (slew_ns > 0) {
            epoch_offset_ns += step;
            slew_ns -= (int64_t)step;
        } else {
            epoch_offset_ns -= step;
            slew_ns += (int64_t)step;
        }
    }
    slew_at = now;
}

int64_t rtc_epoch_seconds(int year, int month, int day, int hour, int minute, int second)
{
    return date_days_from_civil(year, month, day) * 86400 + hour * 3600 + minute * 60 + second;
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
    rtc_set_realtime_ns((uint64_t)seconds * 1000000000);
    klog_info("%04d-%02d-%02d %02d:%02d:%02d UTC, epoch %lld", a.year, a.month, a.day,
              a.hour, a.minute, a.second, (long long)seconds);
}

uint64_t rtc_realtime_ns(void)
{
    spin_lock(&clock_lock);
    uint64_t now = timer_ns();
    slew_locked(now);
    uint64_t t = now + epoch_offset_ns;
    spin_unlock(&clock_lock);
    return t;
}

void rtc_set_realtime_ns(uint64_t ns)
{
    spin_lock(&clock_lock);
    uint64_t now = timer_ns();
    epoch_offset_ns = ns > now ? ns - now : 0;
    slew_ns = 0;
    slew_at = now;
    spin_unlock(&clock_lock);
}

int64_t rtc_adjust(const int64_t *delta)
{
    spin_lock(&clock_lock);
    slew_locked(timer_ns());
    int64_t old = slew_ns;
    if (delta)
        slew_ns = *delta;
    spin_unlock(&clock_lock);
    return old;
}
