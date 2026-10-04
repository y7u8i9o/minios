/* Time zones (L2, docs/design/time.md).
 *
 * The zone is the POSIX rule or the zone file that TZ names, or the file
 * /etc/localtime when TZ is not set, or UTC.  A zone file is a TZif file of
 * version 1, 2 or 3 (RFC 8536): its transitions give the offset before the
 * last transition, and the rule of its footer gives the offset after it.
 * tzset reads the zone again when TZ changes or when /etc/localtime is
 * replaced by another file or modified.  The zone state is protected by
 * lock. */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include "../thread/tcb.h"

#define ZONE_DIR "/usr/share/zoneinfo/"
#define ZONE_FILE_MAX 65536

/* One end of the daylight saving period of a POSIX rule. */
struct rule_date {
    char kind;                  /* 'J' Julian day without Feb 29, 'D' zero based day, 'M' month week day */
    int day, month, week, wday;
    long time;                  /* local seconds after midnight, may be negative or above a day */
};

struct rule {
    char std[16], dst[16];
    long std_off, dst_off;      /* seconds east of UTC */
    int has_dst;
    struct rule_date start, end;
};

struct type {
    long off;                   /* seconds east of UTC */
    int isdst;
    char abbr[16];
};

/* The current zone, protected by lock. */
static struct {
    char key[256];              /* the TZ value it was read for, or "" for /etc/localtime */
    dev_t dev;                  /* the identity of /etc/localtime when it was read */
    ino_t ino;
    time_t mtime;
    int valid;
    int ntrans;
    int64_t *trans;
    unsigned char *trans_type;
    int ntypes;
    struct type *types;
    int has_rule;
    struct rule rule;
} zone;

static struct __libc_lock lock = __LIBC_LOCK_INIT;

static char std_name[16] = "UTC", dst_name[16] = "UTC";
char *tzname[2] = { std_name, dst_name };
long timezone;
int daylight;

/* The functions below parse POSIX rules such as CET-1CEST,M3.5.0,M10.5.0/3. */

static const char *parse_name(const char *s, char *out, size_t size)
{
    size_t n = 0;
    if (*s == '<') {
        s++;
        while (*s && *s != '>' && n + 1 < size)
            out[n++] = *s++;
        if (*s != '>')
            return NULL;
        s++;
    } else {
        while (((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z')) && n + 1 < size)
            out[n++] = *s++;
        if (n < 3)
            return NULL;
    }
    out[n] = '\0';
    return s;
}

/* parse_time reads [+-]hh[:mm[:ss]] with hours up to 167. */
static const char *parse_time(const char *s, long *secs)
{
    int sign = 1;
    if (*s == '+' || *s == '-')
        sign = *s++ == '-' ? -1 : 1;
    if (*s < '0' || *s > '9')
        return NULL;
    long h = 0, m = 0, sec = 0;
    while (*s >= '0' && *s <= '9')
        h = h * 10 + (*s++ - '0');
    if (*s == ':') {
        s++;
        while (*s >= '0' && *s <= '9')
            m = m * 10 + (*s++ - '0');
        if (*s == ':') {
            s++;
            while (*s >= '0' && *s <= '9')
                sec = sec * 10 + (*s++ - '0');
        }
    }
    if (h > 167 || m > 59 || sec > 59)
        return NULL;
    *secs = sign * (h * 3600 + m * 60 + sec);
    return s;
}

static const char *parse_number(const char *s, int *out)
{
    if (*s < '0' || *s > '9')
        return NULL;
    int v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    *out = v;
    return s;
}

static const char *parse_date(const char *s, struct rule_date *d)
{
    d->time = 7200;
    if (*s == 'J') {
        d->kind = 'J';
        if (!(s = parse_number(s + 1, &d->day)) || d->day < 1 || d->day > 365)
            return NULL;
    } else if (*s == 'M') {
        d->kind = 'M';
        if (!(s = parse_number(s + 1, &d->month)) || *s++ != '.' || !(s = parse_number(s, &d->week)) ||
            *s++ != '.' || !(s = parse_number(s, &d->wday)))
            return NULL;
        if (d->month < 1 || d->month > 12 || d->week < 1 || d->week > 5 || d->wday > 6)
            return NULL;
    } else {
        d->kind = 'D';
        if (!(s = parse_number(s, &d->day)) || d->day > 365)
            return NULL;
    }
    if (*s == '/' && !(s = parse_time(s + 1, &d->time)))
        return NULL;
    return s;
}

static int parse_rule(const char *s, struct rule *r)
{
    memset(r, 0, sizeof *r);
    long off;
    if (!(s = parse_name(s, r->std, sizeof r->std)) || !(s = parse_time(s, &off)))
        return -1;
    r->std_off = -off;
    if (!*s)
        return 0;
    if (!(s = parse_name(s, r->dst, sizeof r->dst)))
        return -1;
    r->has_dst = 1;
    r->dst_off = r->std_off + 3600;
    if (*s && *s != ',') {
        if (!(s = parse_time(s, &off)))
            return -1;
        r->dst_off = -off;
    }
    if (!*s) {
        /* A rule without dates takes the dates of the United States. */
        s = ",M3.2.0,M11.1.0";
    }
    if (*s++ != ',' || !(s = parse_date(s, &r->start)) || *s++ != ',' || !(s = parse_date(s, &r->end)) || *s)
        return -1;
    return 0;
}

/* The functions below compute the transitions of a rule. */

static int is_leap(long y)
{
    return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
}

/* days_from_civil returns the days from 1970-01-01 to y-m-d. */
static long days_from_civil(long y, int m, int d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* transition returns the UTC time of a rule date in year y, for a clock
 * that runs at the offset off. */
static int64_t transition(const struct rule_date *d, long y, long off)
{
    static const int month_days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    long days;
    if (d->kind == 'J') {
        days = days_from_civil(y, 1, 1) + d->day - 1 + (is_leap(y) && d->day > 59);
    } else if (d->kind == 'D') {
        days = days_from_civil(y, 1, 1) + d->day;
    } else {
        long first = days_from_civil(y, d->month, 1);
        int wday1 = (int)(((first % 7) + 11) % 7);         /* 1970-01-01 was a Thursday */
        int day = 1 + (d->wday - wday1 + 7) % 7 + (d->week - 1) * 7;
        int length = month_days[d->month - 1] + (d->month == 2 && is_leap(y));
        while (day > length)
            day -= 7;
        days = first + day - 1;
    }
    return (int64_t)days * 86400 + d->time - off;
}

static long year_of(int64_t t)
{
    int64_t days = t / 86400 - (t % 86400 < 0);
    long y = (long)(1970 + days / 365);
    while (days_from_civil(y, 1, 1) > days)
        y--;
    while (days_from_civil(y + 1, 1, 1) <= days)
        y++;
    return y;
}

static int rule_isdst(const struct rule *r, int64_t t)
{
    if (!r->has_dst)
        return 0;
    long y = year_of(t);
    int64_t start = transition(&r->start, y, r->std_off), end = transition(&r->end, y, r->dst_off);
    if (start < end)
        return t >= start && t < end;
    return !(t >= end && t < start);
}

/* The functions below read zone files. */

static int64_t be(const unsigned char *p, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n; i++)
        v = v << 8 | p[i];
    if (n == 4)
        return (int32_t)(uint32_t)v;
    return (int64_t)v;
}

static void clear_zone(void)
{
    free(zone.trans);
    free(zone.trans_type);
    free(zone.types);
    zone.trans = NULL;
    zone.trans_type = NULL;
    zone.types = NULL;
    zone.ntrans = zone.ntypes = 0;
    zone.has_rule = 0;
}

/* parse_tzif reads the transitions, the types and the footer of a TZif
 * file.  Returns 0, or -1 for a malformed file. */
static int parse_tzif(const unsigned char *p, size_t len)
{
    if (len < 44 || memcmp(p, "TZif", 4) != 0)
        return -1;
    int version = p[4] ? p[4] - '0' : 1;
    size_t at = 0;
    int width = 4;
    for (int pass = 0; pass < (version >= 2 ? 2 : 1); pass++) {
        if (at + 44 > len || memcmp(p + at, "TZif", 4) != 0)
            return -1;
        const unsigned char *h = p + at + 20;
        long isutcnt = (long)be(h, 4), isstdcnt = (long)be(h + 4, 4), leapcnt = (long)be(h + 8, 4);
        long timecnt = (long)be(h + 12, 4), typecnt = (long)be(h + 16, 4), charcnt = (long)be(h + 20, 4);
        at += 44;
        size_t data = (size_t)(timecnt * width + timecnt + typecnt * 6 + charcnt + leapcnt * (width + 4) +
                               isstdcnt + isutcnt);
        if (typecnt < 1 || at + data > len)
            return -1;
        if (version >= 2 && pass == 0) {
            at += data;
            width = 8;
            continue;
        }
        clear_zone();
        zone.trans = malloc(sizeof *zone.trans * (size_t)(timecnt + 1));
        zone.trans_type = malloc((size_t)timecnt + 1);
        zone.types = malloc(sizeof *zone.types * (size_t)typecnt);
        if (!zone.trans || !zone.trans_type || !zone.types)
            return -1;
        const unsigned char *q = p + at;
        for (long i = 0; i < timecnt; i++)
            zone.trans[i] = be(q + i * width, width);
        q += timecnt * width;
        for (long i = 0; i < timecnt; i++)
            zone.trans_type[i] = q[i] < typecnt ? q[i] : 0;
        q += timecnt;
        const unsigned char *chars = q + typecnt * 6;
        for (long i = 0; i < typecnt; i++) {
            zone.types[i].off = (long)be(q + i * 6, 4);
            zone.types[i].isdst = q[i * 6 + 4];
            int idx = q[i * 6 + 5];
            zone.types[i].abbr[0] = '\0';
            if (idx < charcnt) {
                size_t n = strnlen((const char *)chars + idx, (size_t)(charcnt - idx));
                if (n >= sizeof zone.types[i].abbr)
                    n = sizeof zone.types[i].abbr - 1;
                memcpy(zone.types[i].abbr, chars + idx, n);
                zone.types[i].abbr[n] = '\0';
            }
        }
        zone.ntrans = (int)timecnt;
        zone.ntypes = (int)typecnt;
        at += data;
    }
    /* The footer of version 2 and later: a newline, a rule and a newline. */
    if (version >= 2 && at < len && p[at] == '\n') {
        char footer[128];
        size_t n = 0;
        at++;
        while (at < len && p[at] != '\n' && n + 1 < sizeof footer)
            footer[n++] = (char)p[at++];
        footer[n] = '\0';
        if (n && parse_rule(footer, &zone.rule) == 0)
            zone.has_rule = 1;
    }
    return 0;
}

static int load_file(const char *path)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    unsigned char *buf = malloc(ZONE_FILE_MAX);
    if (!buf) {
        close(fd);
        return -1;
    }
    size_t len = 0;
    ssize_t n;
    while (len < ZONE_FILE_MAX && (n = read(fd, buf + len, ZONE_FILE_MAX - len)) > 0)
        len += (size_t)n;
    close(fd);
    int r = parse_tzif(buf, len);
    free(buf);
    return r;
}

static void set_utc(void)
{
    clear_zone();
    parse_rule("UTC0", &zone.rule);
    zone.has_rule = 1;
}

/* load reads the zone of a TZ value: a rule, or else a file named
 * relative to /usr/share/zoneinfo or by an absolute path.  A leading colon
 * selects a file.  An unknown value selects UTC. */
static void load(const char *tz)
{
    clear_zone();
    if (*tz != ':' && parse_rule(tz, &zone.rule) == 0) {
        zone.has_rule = 1;
        return;
    }
    if (*tz == ':')
        tz++;
    if (*tz && !strstr(tz, "..")) {
        char path[256];
        if (*tz == '/') {
            strlcpy(path, tz, sizeof path);
        } else {
            strlcpy(path, ZONE_DIR, sizeof path);
            strlcat(path, tz, sizeof path);
        }
        if (load_file(path) == 0)
            return;
    }
    set_utc();
}

/* The offset, the daylight saving flag and the abbreviation at a time. */
struct local {
    long off;
    int isdst;
    const char *abbr;
};

static struct local lookup(int64_t t)
{
    struct local l = { 0, 0, "UTC" };
    if (zone.ntrans > 0 && (t < zone.trans[zone.ntrans - 1] || !zone.has_rule)) {
        const struct type *ty = &zone.types[0];
        if (t >= zone.trans[0]) {
            int lo = 0, hi = zone.ntrans - 1;
            while (lo < hi) {
                int mid = (lo + hi + 1) / 2;
                if (zone.trans[mid] <= t)
                    lo = mid;
                else
                    hi = mid - 1;
            }
            ty = &zone.types[zone.trans_type[lo]];
        } else {
            for (int i = 0; i < zone.ntypes; i++)
                if (!zone.types[i].isdst) {
                    ty = &zone.types[i];
                    break;
                }
        }
        l.off = ty->off;
        l.isdst = ty->isdst;
        l.abbr = ty->abbr;
        return l;
    }
    if (zone.has_rule) {
        l.isdst = rule_isdst(&zone.rule, t);
        l.off = l.isdst ? zone.rule.dst_off : zone.rule.std_off;
        l.abbr = l.isdst ? zone.rule.dst : zone.rule.std;
    } else if (zone.ntypes > 0) {
        l.off = zone.types[zone.ntypes - 1].off;
        l.isdst = zone.types[zone.ntypes - 1].isdst;
        l.abbr = zone.types[zone.ntypes - 1].abbr;
    }
    return l;
}

/* update reads the zone again when TZ or /etc/localtime changed and sets
 * tzname, timezone and daylight.  The caller has acquired lock. */
static void update(void)
{
    const char *tz = getenv("TZ");
    if (tz) {
        if (!zone.valid || strcmp(zone.key, tz) != 0 || zone.ino) {
            load(tz);
            strlcpy(zone.key, tz, sizeof zone.key);
            zone.ino = 0;
            zone.valid = 1;
        }
    } else {
        struct stat st;
        int found = stat("/etc/localtime", &st) == 0;
        if (!zone.valid || zone.key[0] || !found != !zone.ino || (found && (st.st_ino != zone.ino ||
            st.st_dev != zone.dev || st.st_mtime != zone.mtime))) {
            if (!found || load_file("/etc/localtime") < 0)
                set_utc();
            zone.key[0] = '\0';
            zone.ino = found ? st.st_ino : 0;
            zone.dev = found ? st.st_dev : 0;
            zone.mtime = found ? st.st_mtime : 0;
            zone.valid = 1;
        }
    }
    if (zone.has_rule) {
        strlcpy(std_name, zone.rule.std, sizeof std_name);
        strlcpy(dst_name, zone.rule.has_dst ? zone.rule.dst : zone.rule.std, sizeof dst_name);
        timezone = -zone.rule.std_off;
        daylight = zone.rule.has_dst;
    } else if (zone.ntypes > 0) {
        struct local now = lookup(zone.trans[zone.ntrans > 0 ? zone.ntrans - 1 : 0]);
        strlcpy(std_name, now.abbr, sizeof std_name);
        strlcpy(dst_name, now.abbr, sizeof dst_name);
        timezone = -now.off;
        daylight = 0;
        for (int i = 0; i < zone.ntypes; i++)
            if (zone.types[i].isdst) {
                strlcpy(dst_name, zone.types[i].abbr, sizeof dst_name);
                daylight = 1;
            }
    }
}

void tzset(void)
{
    __libc_lock_lock(&lock);
    update();
    __libc_lock_unlock(&lock);
}

struct tm *localtime_r(const time_t *t, struct tm *out)
{
    __libc_lock_lock(&lock);
    update();
    struct local l = lookup((int64_t)*t);
    __libc_lock_unlock(&lock);
    time_t shifted = *t + l.off;
    gmtime_r(&shifted, out);
    out->tm_isdst = l.isdst;
    return out;
}

struct tm *localtime(const time_t *t)
{
    static struct tm shared;
    return localtime_r(t, &shared);
}

/* mktime converts a local time.  A time that occurs twice when the clocks
 * go back takes the offset that tm_isdst names, when it is not negative.
 * A time that does not occur when the clocks go forward is converted with
 * the offset before the change and normalized. */
time_t mktime(struct tm *tm)
{
    int hint = tm->tm_isdst;
    struct tm copy = *tm;
    int64_t local = (int64_t)timegm(&copy);
    __libc_lock_lock(&lock);
    update();
    struct local guess = lookup(local - (zone.has_rule ? zone.rule.std_off : 0));
    int64_t t = local - guess.off;
    struct local at = lookup(t);
    if (at.off != guess.off) {
        t = local - at.off;
        at = lookup(t);
    }
    if (hint >= 0 && at.isdst != (hint > 0) && zone.has_rule && zone.rule.has_dst) {
        long other = at.isdst ? zone.rule.std_off : zone.rule.dst_off;
        struct local alt = lookup(local - other);
        if (alt.isdst == (hint > 0) && alt.off == other)
            t = local - other;
    }
    __libc_lock_unlock(&lock);
    time_t result = (time_t)t;
    localtime_r(&result, tm);
    return result;
}

/* __tz_for_tm gives the offset and the abbreviation of the local time tm
 * for %z and %Z of strftime. */
void __tz_for_tm(const struct tm *tm, long *off, char *abbr, size_t size)
{
    struct tm copy = *tm;
    time_t t = mktime(&copy);
    __libc_lock_lock(&lock);
    struct local l = lookup((int64_t)t);
    *off = l.off;
    strlcpy(abbr, l.abbr, size);
    __libc_lock_unlock(&lock);
}
