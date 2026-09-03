/* CMOS real time clock (M35). The clock is read once at boot, when the
 * monotonic timer has just started, and the difference between the two
 * becomes the epoch offset of CLOCK_REALTIME; the clock is never read
 * again, so a slow CMOS access cannot disturb a running system. QEMU
 * keeps the CMOS clock in UTC. */
#define KLOG_SUBSYS "rtc"
#include <drivers/rtc.h>
#include <drivers/timer.h>
#include <arch/io.h>
#include <klog.h>

#define CMOS_ADDRESS 0x70
#define CMOS_DATA    0x71
#define RTC_SECONDS  0x00
#define RTC_MINUTES  0x02
#define RTC_HOURS    0x04
#define RTC_DAY      0x07
#define RTC_MONTH    0x08
#define RTC_YEAR     0x09
#define RTC_CENTURY  0x32
#define RTC_STATUS_A 0x0a
#define RTC_STATUS_B 0x0b
#define STATUS_A_UPDATING 0x80
#define STATUS_B_24H      0x02
#define STATUS_B_BINARY   0x04

/* Written by rtc_init and clock_settime, read by clock_gettime; a single
 * aligned word, so no lock. */
static uint64_t epoch_offset_ns;

static uint8_t cmos_read(uint8_t reg)
{
    outb(CMOS_ADDRESS, (uint8_t)(0x80 | reg));   /* NMI stays disabled during the access */
    return inb(CMOS_DATA);
}

static int from_bcd(uint8_t v)
{
    return (v >> 4) * 10 + (v & 0x0f);
}

struct rtc_time {
    int second, minute, hour, day, month, year;
};

static void read_registers(struct rtc_time *t)
{
    t->second = cmos_read(RTC_SECONDS);
    t->minute = cmos_read(RTC_MINUTES);
    t->hour = cmos_read(RTC_HOURS);
    t->day = cmos_read(RTC_DAY);
    t->month = cmos_read(RTC_MONTH);
    t->year = cmos_read(RTC_YEAR);
}

static bool same(const struct rtc_time *a, const struct rtc_time *b)
{
    return a->second == b->second && a->minute == b->minute && a->hour == b->hour &&
           a->day == b->day && a->month == b->month && a->year == b->year;
}

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
    struct rtc_time a, b;
    /* Read until two consecutive reads agree and no update is in progress. */
    for (int i = 0; i < 100; i++) {
        while (cmos_read(RTC_STATUS_A) & STATUS_A_UPDATING)
            ;
        read_registers(&a);
        read_registers(&b);
        if (same(&a, &b))
            break;
    }
    uint8_t status_b = cmos_read(RTC_STATUS_B);
    int century = cmos_read(RTC_CENTURY);
    uint8_t raw_hour = (uint8_t)a.hour;
    if (!(status_b & STATUS_B_BINARY)) {
        a.second = from_bcd((uint8_t)a.second);
        a.minute = from_bcd((uint8_t)a.minute);
        a.hour = from_bcd(raw_hour & 0x7f);
        a.day = from_bcd((uint8_t)a.day);
        a.month = from_bcd((uint8_t)a.month);
        a.year = from_bcd((uint8_t)a.year);
        century = from_bcd((uint8_t)century);
    } else {
        a.hour = raw_hour & 0x7f;
    }
    if (!(status_b & STATUS_B_24H) && (raw_hour & 0x80))
        a.hour = (a.hour % 12) + 12;   /* PM in 12 hour mode */
    int year = (century >= 19 && century <= 99 ? century * 100 : 2000) + a.year;
    int64_t seconds = rtc_epoch_seconds(year, a.month, a.day, a.hour, a.minute, a.second);
    if (year < 2000 || a.month < 1 || a.month > 12 || a.day < 1 || a.day > 31) {
        klog_warn("implausible clock %04d-%02d-%02d, realtime starts at 2000-01-01", year, a.month, a.day);
        seconds = rtc_epoch_seconds(2000, 1, 1, 0, 0, 0);
    }
    epoch_offset_ns = (uint64_t)seconds * 1000000000 - timer_ns();
    klog_info("%04d-%02d-%02d %02d:%02d:%02d UTC, epoch %lld", year, a.month, a.day,
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
