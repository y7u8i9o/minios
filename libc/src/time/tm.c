/* Calendar conversion and formatting (M35). Time zones are in tz.c. The civil date
 * arithmetic follows the era based algorithms of Howard Hinnant, which
 * work for any year of the proleptic Gregorian calendar. */
#include <time.h>
#include <stdio.h>
#include <string.h>
#include "../locale/locale_impl.h"

static const char *const day_names[7] = {
    "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"
};
static const char *const month_names[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};
static const int days_before_month[2][12] = {
    { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 },
    { 0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335 },
};

static int is_leap(long y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

/* Days from 1970-01-01 to y-m-d (m 1 to 12). */
static long days_from_civil(long y, int m, int d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(long z, long *y, int *m, int *d)
{
    z += 719468;
    long era = (z >= 0 ? z : z - 146096) / 146097;
    long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long yy = yoe + era * 400;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = yy + (*m <= 2);
}

struct tm *gmtime_r(const time_t *t, struct tm *out)
{
    long secs = (long)*t;
    long days = secs / 86400;
    long rem = secs % 86400;
    if (rem < 0) {
        rem += 86400;
        days--;
    }
    long year;
    int month, day;
    civil_from_days(days, &year, &month, &day);
    out->tm_sec = (int)(rem % 60);
    out->tm_min = (int)(rem / 60 % 60);
    out->tm_hour = (int)(rem / 3600);
    out->tm_mday = day;
    out->tm_mon = month - 1;
    out->tm_year = (int)(year - 1900);
    out->tm_wday = (int)(((days % 7) + 11) % 7);   /* 1970-01-01 was a Thursday */
    out->tm_yday = days_before_month[is_leap(year)][month - 1] + day - 1;
    out->tm_isdst = 0;
    return out;
}

struct tm *gmtime(const time_t *t)
{
    static struct tm shared;
    return gmtime_r(t, &shared);
}

/* Normalizes the fields as mktime does. */
time_t timegm(struct tm *tm)
{
    long year = tm->tm_year + 1900L;
    long mon = tm->tm_mon;
    year += mon / 12;
    mon %= 12;
    if (mon < 0) {
        mon += 12;
        year--;
    }
    long days = days_from_civil(year, (int)mon + 1, 1) + tm->tm_mday - 1;
    long secs = days * 86400 + (long)tm->tm_hour * 3600 + (long)tm->tm_min * 60 + tm->tm_sec;
    time_t t = (time_t)secs;
    gmtime_r(&t, tm);
    return t;
}

void __tz_for_tm(const struct tm *tm, long *off, char *abbr, size_t size);

/* The output of strftime: characters past size are counted but not
 * stored. */
struct out {
    char *buf;
    size_t size, at;
};

static void put(struct out *o, const char *s)
{
    size_t n = strlen(s);
    if (o->at + n < o->size)
        memcpy(o->buf + o->at, s, n);
    o->at += n;
}

static int weeks_in_year(long y)
{
    long p = (y + y / 4 - y / 100 + y / 400) % 7, q = ((y - 1) + (y - 1) / 4 - (y - 1) / 100 + (y - 1) / 400) % 7;
    return p == 4 || q == 3 ? 53 : 52;
}

/* iso_week returns the ISO 8601 week number and stores its year. */
static int iso_week(const struct tm *tm, long *year)
{
    long y = tm->tm_year + 1900L;
    int wday = tm->tm_wday == 0 ? 7 : tm->tm_wday;
    int week = (tm->tm_yday + 1 - wday + 10) / 7;
    if (week < 1) {
        y--;
        week = weeks_in_year(y);
    } else if (week > weeks_in_year(y)) {
        y++;
        week = 1;
    }
    *year = y;
    return week;
}

static void format(struct out *o, const char *f, const struct tm *tm, locale_t loc, int depth)
{
    char item[64];
    int hour12 = tm->tm_hour % 12 ? tm->tm_hour % 12 : 12;
    int wday = ((tm->tm_wday % 7) + 7) % 7, mon = ((tm->tm_mon % 12) + 12) % 12;
    long year;
    for (; *f; f++) {
        if (*f != '%') {
            if (o->at + 1 < o->size)
                o->buf[o->at] = *f;
            o->at++;
            continue;
        }
        f++;
        /* E selects an era form, which no locale has. O selects the
         * month names used without a day for b, B and h. */
        int alt = 0;
        if (*f == 'E') {
            f++;
        } else if (*f == 'O') {
            alt = 1;
            f++;
        }
        const char *sub = NULL;
        item[0] = '\0';
        switch (*f) {
        case 'a': sub = __locale_item(loc, ABDAY_1 + wday); break;
        case 'A': sub = __locale_item(loc, DAY_1 + wday); break;
        case 'b': case 'h': sub = __locale_item(loc, (alt ? _NL_ABALTMON_1 : ABMON_1) + mon); break;
        case 'B': sub = __locale_item(loc, (alt ? ALTMON_1 : MON_1) + mon); break;
        case 'c': if (depth < 4) format(o, __locale_item(loc, D_T_FMT), tm, loc, depth + 1); continue;
        case 'C': snprintf(item, sizeof item, "%02d", (tm->tm_year + 1900) / 100); break;
        case 'd': snprintf(item, sizeof item, "%02d", tm->tm_mday); break;
        case 'D': if (depth < 4) format(o, "%m/%d/%y", tm, loc, depth + 1); continue;
        case 'e': snprintf(item, sizeof item, "%2d", tm->tm_mday); break;
        case 'F': snprintf(item, sizeof item, "%04d-%02d-%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday); break;
        case 'g': iso_week(tm, &year); snprintf(item, sizeof item, "%02ld", year % 100); break;
        case 'G': iso_week(tm, &year); snprintf(item, sizeof item, "%ld", year); break;
        case 'H': snprintf(item, sizeof item, "%02d", tm->tm_hour); break;
        case 'I': snprintf(item, sizeof item, "%02d", hour12); break;
        case 'j': snprintf(item, sizeof item, "%03d", tm->tm_yday + 1); break;
        case 'k': snprintf(item, sizeof item, "%2d", tm->tm_hour); break;
        case 'l': snprintf(item, sizeof item, "%2d", hour12); break;
        case 'm': snprintf(item, sizeof item, "%02d", tm->tm_mon + 1); break;
        case 'M': snprintf(item, sizeof item, "%02d", tm->tm_min); break;
        case 'n': sub = "\n"; break;
        case 'p': sub = __locale_item(loc, tm->tm_hour < 12 ? AM_STR : PM_STR); break;
        case 'P':
            strlcpy(item, __locale_item(loc, tm->tm_hour < 12 ? AM_STR : PM_STR), sizeof item);
            for (char *c = item; *c; c++)
                if (*c >= 'A' && *c <= 'Z')
                    *c = (char)(*c - 'A' + 'a');
            break;
        case 'r': {
            const char *ampm = __locale_item(loc, T_FMT_AMPM);
            if (depth < 4)
                format(o, *ampm ? ampm : "%I:%M:%S %p", tm, loc, depth + 1);
            continue;
        }
        case 'R': snprintf(item, sizeof item, "%02d:%02d", tm->tm_hour, tm->tm_min); break;
        case 's': {
            struct tm copy = *tm;
            snprintf(item, sizeof item, "%lld", (long long)mktime(&copy));
            break;
        }
        case 'S': snprintf(item, sizeof item, "%02d", tm->tm_sec); break;
        case 't': sub = "\t"; break;
        case 'T': snprintf(item, sizeof item, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec); break;
        case 'u': snprintf(item, sizeof item, "%d", wday == 0 ? 7 : wday); break;
        case 'U': snprintf(item, sizeof item, "%02d", (tm->tm_yday + 7 - wday) / 7); break;
        case 'V': snprintf(item, sizeof item, "%02d", iso_week(tm, &year)); break;
        case 'w': snprintf(item, sizeof item, "%d", wday); break;
        case 'W': snprintf(item, sizeof item, "%02d", (tm->tm_yday + 7 - (wday + 6) % 7) / 7); break;
        case 'x': if (depth < 4) format(o, __locale_item(loc, D_FMT), tm, loc, depth + 1); continue;
        case 'X': if (depth < 4) format(o, __locale_item(loc, T_FMT), tm, loc, depth + 1); continue;
        case 'y': snprintf(item, sizeof item, "%02d", (tm->tm_year + 1900) % 100); break;
        case 'Y': snprintf(item, sizeof item, "%d", tm->tm_year + 1900); break;
        case 'z': case 'Z': {
            long off;
            char abbr[16];
            __tz_for_tm(tm, &off, abbr, sizeof abbr);
            if (*f == 'Z') {
                strlcpy(item, abbr, sizeof item);
            } else {
                long a = off < 0 ? -off : off;
                snprintf(item, sizeof item, "%c%02ld%02ld", off < 0 ? '-' : '+', a / 3600, a / 60 % 60);
            }
            break;
        }
        case '%': sub = "%"; break;
        case '\0': f--; continue;
        default: snprintf(item, sizeof item, "%%%c", *f); break;
        }
        put(o, sub ? sub : item);
    }
}

size_t strftime_l(char *buf, size_t size, const char *fmt, const struct tm *tm, locale_t loc)
{
    if (loc == LC_GLOBAL_LOCALE)
        loc = &__global_locale;
    struct out o = { buf, size, 0 };
    format(&o, fmt, tm, loc, 0);
    if (o.at < size) {
        buf[o.at] = '\0';
        return o.at;
    }
    if (size)
        buf[0] = '\0';
    return 0;
}

size_t strftime(char *buf, size_t size, const char *fmt, const struct tm *tm)
{
    return strftime_l(buf, size, fmt, tm, __locale_current());
}

char *asctime_r(const struct tm *tm, char *buf)
{
    snprintf(buf, 26, "%.3s %.3s %2d %02d:%02d:%02d %d\n", day_names[tm->tm_wday % 7],
             month_names[tm->tm_mon % 12], tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec,
             tm->tm_year + 1900);
    return buf;
}

char *asctime(const struct tm *tm)
{
    static char shared[26];
    return asctime_r(tm, shared);
}

char *ctime_r(const time_t *t, char *buf)
{
    struct tm tm;
    return asctime_r(gmtime_r(t, &tm), buf);
}

char *ctime(const time_t *t)
{
    struct tm tm;
    return asctime(gmtime_r(t, &tm));
}
