/* Calendar conversion and formatting (M35), UTC only. The civil date
 * arithmetic follows the era based algorithms of Howard Hinnant, which
 * work for any year of the proleptic Gregorian calendar. */
#include <time.h>
#include <stdio.h>
#include <string.h>

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

static char utc[] = "UTC";
char *tzname[2] = { utc, utc };
long timezone;
int daylight;

void tzset(void)
{
    /* The kernel clock and every calendar conversion use UTC. */
}

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

struct tm *localtime_r(const time_t *t, struct tm *out)
{
    return gmtime_r(t, out);
}

struct tm *localtime(const time_t *t)
{
    return gmtime(t);
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

time_t mktime(struct tm *tm)
{
    return timegm(tm);
}

static size_t put(char *buf, size_t size, size_t at, const char *s)
{
    size_t n = strlen(s);
    if (at + n < size)
        memcpy(buf + at, s, n);
    return at + n;
}

size_t strftime(char *buf, size_t size, const char *format, const struct tm *tm)
{
    char item[64];
    size_t at = 0;
    int hour12 = tm->tm_hour % 12 ? tm->tm_hour % 12 : 12;
    for (const char *f = format; *f; f++) {
        if (*f != '%') {
            if (at + 1 < size)
                buf[at] = *f;
            at++;
            continue;
        }
        f++;
        switch (*f) {
        case 'a': snprintf(item, sizeof item, "%.3s", day_names[tm->tm_wday % 7]); break;
        case 'A': snprintf(item, sizeof item, "%s", day_names[tm->tm_wday % 7]); break;
        case 'b': case 'h': snprintf(item, sizeof item, "%.3s", month_names[tm->tm_mon % 12]); break;
        case 'B': snprintf(item, sizeof item, "%s", month_names[tm->tm_mon % 12]); break;
        case 'c': snprintf(item, sizeof item, "%.3s %.3s %2d %02d:%02d:%02d %d", day_names[tm->tm_wday % 7],
                           month_names[tm->tm_mon % 12], tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec,
                           tm->tm_year + 1900); break;
        case 'C': snprintf(item, sizeof item, "%02d", (tm->tm_year + 1900) / 100); break;
        case 'd': snprintf(item, sizeof item, "%02d", tm->tm_mday); break;
        case 'D': snprintf(item, sizeof item, "%02d/%02d/%02d", tm->tm_mon + 1, tm->tm_mday, (tm->tm_year + 1900) % 100); break;
        case 'e': snprintf(item, sizeof item, "%2d", tm->tm_mday); break;
        case 'F': snprintf(item, sizeof item, "%04d-%02d-%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday); break;
        case 'H': snprintf(item, sizeof item, "%02d", tm->tm_hour); break;
        case 'I': snprintf(item, sizeof item, "%02d", hour12); break;
        case 'j': snprintf(item, sizeof item, "%03d", tm->tm_yday + 1); break;
        case 'm': snprintf(item, sizeof item, "%02d", tm->tm_mon + 1); break;
        case 'M': snprintf(item, sizeof item, "%02d", tm->tm_min); break;
        case 'n': snprintf(item, sizeof item, "\n"); break;
        case 'p': snprintf(item, sizeof item, "%s", tm->tm_hour < 12 ? "AM" : "PM"); break;
        case 'R': snprintf(item, sizeof item, "%02d:%02d", tm->tm_hour, tm->tm_min); break;
        case 'S': snprintf(item, sizeof item, "%02d", tm->tm_sec); break;
        case 't': snprintf(item, sizeof item, "\t"); break;
        case 'T': case 'X': snprintf(item, sizeof item, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec); break;
        case 'u': snprintf(item, sizeof item, "%d", tm->tm_wday == 0 ? 7 : tm->tm_wday); break;
        case 'w': snprintf(item, sizeof item, "%d", tm->tm_wday); break;
        case 'x': snprintf(item, sizeof item, "%02d/%02d/%02d", tm->tm_mon + 1, tm->tm_mday, (tm->tm_year + 1900) % 100); break;
        case 'y': snprintf(item, sizeof item, "%02d", (tm->tm_year + 1900) % 100); break;
        case 'Y': snprintf(item, sizeof item, "%d", tm->tm_year + 1900); break;
        case 'z': snprintf(item, sizeof item, "+0000"); break;
        case 'Z': snprintf(item, sizeof item, "UTC"); break;
        case '%': snprintf(item, sizeof item, "%%"); break;
        case '\0': f--; item[0] = '\0'; break;
        default: snprintf(item, sizeof item, "%%%c", *f); break;
        }
        at = put(buf, size, at, item);
    }
    if (at < size) {
        buf[at] = '\0';
        return at;
    }
    if (size)
        buf[0] = '\0';
    return 0;
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
