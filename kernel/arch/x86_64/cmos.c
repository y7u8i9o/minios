/* The CMOS real time clock of the PC (M35). QEMU sets it to UTC. */
#include <arch/platform.h>
#include <arch/io.h>
#include <drivers/rtc.h>

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

static uint8_t cmos_read(uint8_t reg)
{
    outb(CMOS_ADDRESS, (uint8_t)(0x80 | reg));   /* NMI remains disabled during the access */
    return inb(CMOS_DATA);
}

static int from_bcd(uint8_t v)
{
    return (v >> 4) * 10 + (v & 0x0f);
}

static void read_registers(struct rtc_date *t)
{
    t->second = cmos_read(RTC_SECONDS);
    t->minute = cmos_read(RTC_MINUTES);
    t->hour = cmos_read(RTC_HOURS);
    t->day = cmos_read(RTC_DAY);
    t->month = cmos_read(RTC_MONTH);
    t->year = cmos_read(RTC_YEAR);
}

static bool same(const struct rtc_date *a, const struct rtc_date *b)
{
    return a->second == b->second && a->minute == b->minute && a->hour == b->hour &&
           a->day == b->day && a->month == b->month && a->year == b->year;
}

void platform_rtc_read(struct rtc_date *d)
{
    struct rtc_date a, b;
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
    a.year += century >= 19 && century <= 99 ? century * 100 : 2000;
    *d = a;
}
