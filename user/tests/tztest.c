/* L2: time zones.  POSIX rules with daylight saving time in both
 * hemispheres, the zone files of /usr/share/zoneinfo, TZ against
 * /etc/localtime, mktime at the changes of the clocks, %z and %Z, and the
 * local time of date and Lua. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("tztest: FAIL " __VA_ARGS__); printf("\n"); } } while (0)

static time_t utc(int y, int mo, int d, int h, int mi, int s)
{
    struct tm tm = { .tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = h, .tm_min = mi, .tm_sec = s };
    return timegm(&tm);
}

static void set_tz(const char *tz)
{
    if (tz)
        setenv("TZ", tz, 1);
    else
        unsetenv("TZ");
    tzset();
}

/* local formats the local time of t with "%Y-%m-%d %H:%M:%S %z %Z" and
 * the daylight saving flag. */
static void local(time_t t, char *buf, size_t size)
{
    struct tm tm;
    localtime_r(&t, &tm);
    char text[80];
    strftime(text, sizeof text, "%Y-%m-%d %H:%M:%S %z %Z", &tm);
    snprintf(buf, size, "%s %d", text, tm.tm_isdst);
}

static void expect_local(time_t t, const char *want, const char *what)
{
    char buf[128];
    local(t, buf, sizeof buf);
    CHECK(strcmp(buf, want) == 0, "%s: %s, expected %s", what, buf, want);
}

static void test_rules(void)
{
    set_tz("CET-1CEST,M3.5.0,M10.5.0/3");
    CHECK(strcmp(tzname[0], "CET") == 0 && strcmp(tzname[1], "CEST") == 0 && timezone == -3600 && daylight == 1,
          "tzset of a rule: %s %s %ld %d", tzname[0], tzname[1], timezone, daylight);
    time_t start = utc(2026, 3, 29, 1, 0, 0), end = utc(2026, 10, 25, 1, 0, 0);
    expect_local(start - 1, "2026-03-29 01:59:59 +0100 CET 0", "before the spring change");
    expect_local(start, "2026-03-29 03:00:00 +0200 CEST 1", "after the spring change");
    expect_local(end - 1, "2026-10-25 02:59:59 +0200 CEST 1", "before the autumn change");
    expect_local(end, "2026-10-25 02:00:00 +0100 CET 0", "after the autumn change");

    struct tm gap = { .tm_year = 126, .tm_mon = 2, .tm_mday = 29, .tm_hour = 2, .tm_min = 30, .tm_isdst = -1 };
    time_t t = mktime(&gap);
    CHECK(t == utc(2026, 3, 29, 1, 30, 0) && gap.tm_hour == 3 && gap.tm_isdst == 1,
          "mktime in the spring gap: %ld %02d:%02d", (long)t, gap.tm_hour, gap.tm_min);
    struct tm twice = { .tm_year = 126, .tm_mon = 9, .tm_mday = 25, .tm_hour = 2, .tm_min = 30, .tm_isdst = 1 };
    t = mktime(&twice);
    CHECK(t == utc(2026, 10, 25, 0, 30, 0), "mktime of the first 02:30 in October: %ld", (long)t);
    twice = (struct tm){ .tm_year = 126, .tm_mon = 9, .tm_mday = 25, .tm_hour = 2, .tm_min = 30, .tm_isdst = 0 };
    t = mktime(&twice);
    CHECK(t == utc(2026, 10, 25, 1, 30, 0), "mktime of the second 02:30 in October: %ld", (long)t);
    struct tm noon = { .tm_year = 126, .tm_mon = 6, .tm_mday = 14, .tm_hour = 12, .tm_isdst = -1 };
    t = mktime(&noon);
    CHECK(t == utc(2026, 7, 14, 10, 0, 0) && noon.tm_wday == 2 && noon.tm_yday == 194, "mktime in summer");

    set_tz("EST5EDT,M3.2.0,M11.1.0");
    expect_local(utc(2026, 3, 8, 7, 0, 0) - 1, "2026-03-08 01:59:59 -0500 EST 0", "New York before spring");
    expect_local(utc(2026, 3, 8, 7, 0, 0), "2026-03-08 03:00:00 -0400 EDT 1", "New York after spring");
    expect_local(utc(2026, 11, 1, 6, 0, 0) - 1, "2026-11-01 01:59:59 -0400 EDT 1", "New York before autumn");
    expect_local(utc(2026, 11, 1, 6, 0, 0), "2026-11-01 01:00:00 -0500 EST 0", "New York after autumn");

    set_tz("AEST-10AEDT,M10.1.0,M4.1.0/3");
    expect_local(utc(2026, 1, 15, 0, 0, 0), "2026-01-15 11:00:00 +1100 AEDT 1", "Sydney in January");
    expect_local(utc(2026, 7, 15, 0, 0, 0), "2026-07-15 10:00:00 +1000 AEST 0", "Sydney in July");

    set_tz("<+0545>-5:45");
    expect_local(0, "1970-01-01 05:45:00 +0545 +0545 0", "a quoted name and minutes");
    set_tz("JST-9");
    CHECK(strcmp(tzname[0], "JST") == 0 && daylight == 0 && timezone == -32400, "a zone without daylight saving");
}

static void test_files(void)
{
    set_tz("Asia/Shanghai");
    expect_local(utc(2026, 1, 5, 6, 0, 0), "2026-01-05 14:00:00 +0800 CST 0", "Asia/Shanghai");
    set_tz(":Asia/Tokyo");
    expect_local(utc(2026, 1, 5, 6, 0, 0), "2026-01-05 15:00:00 +0900 JST 0", ":Asia/Tokyo");
    set_tz("/usr/share/zoneinfo/Europe/Paris");
    expect_local(utc(2026, 7, 1, 12, 0, 0), "2026-07-01 14:00:00 +0200 CEST 1", "an absolute zone path");
    set_tz("America/New_York");
    expect_local(utc(2026, 12, 1, 12, 0, 0), "2026-12-01 07:00:00 -0500 EST 0", "America/New_York");
    set_tz("Asia/Tehran");
    expect_local(utc(2026, 1, 1, 0, 0, 0), "2026-01-01 03:30:00 +0330 +0330 0", "Asia/Tehran");
    set_tz("Europe/Dublin");
    expect_local(utc(2026, 7, 1, 12, 0, 0), "2026-07-01 13:00:00 +0100 IST 0", "Dublin in summer");
    expect_local(utc(2026, 1, 1, 12, 0, 0), "2026-01-01 12:00:00 +0000 GMT 1", "Dublin in winter");
    set_tz("Nowhere/City");
    expect_local(0, "1970-01-01 00:00:00 +0000 UTC 0", "an unknown zone is UTC");
}

static void test_localtime_file(void)
{
    unlink("/etc/localtime");
    set_tz(NULL);
    expect_local(0, "1970-01-01 00:00:00 +0000 UTC 0", "without TZ and /etc/localtime");
    CHECK(symlink("/usr/share/zoneinfo/Asia/Shanghai", "/etc/localtime") == 0, "symlink /etc/localtime");
    expect_local(0, "1970-01-01 08:00:00 +0800 CST 0", "/etc/localtime links to Shanghai");
    unlink("/etc/localtime");
    CHECK(symlink("/usr/share/zoneinfo/Asia/Tokyo", "/etc/localtime") == 0, "symlink /etc/localtime again");
    expect_local(0, "1970-01-01 09:00:00 +0900 JST 0", "a replaced /etc/localtime is read again");
    set_tz("UTC0");
    expect_local(0, "1970-01-01 00:00:00 +0000 UTC 0", "TZ takes precedence over /etc/localtime");
    set_tz(NULL);
    expect_local(0, "1970-01-01 09:00:00 +0900 JST 0", "unsetting TZ returns to /etc/localtime");
    unlink("/etc/localtime");
}

static int run(char *const argv[], char *const env[], char *out, size_t size)
{
    int p[2];
    if (pipe(p) < 0)
        return -1;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(p[1], 1);
        close(p[0]);
        close(p[1]);
        execve(argv[0], argv, env);
        _exit(127);
    }
    close(p[1]);
    size_t n = 0;
    ssize_t r;
    while (n < size - 1 && (r = read(p[0], out + n, size - 1 - n)) > 0)
        n += (size_t)r;
    out[n] = '\0';
    close(p[0]);
    int status;
    waitpid(pid, &status, 0);
    return status;
}

static void test_programs(void)
{
    char out[256];
    run((char *const[]){ "/bin/date", "-u", "+%Z %z", NULL }, (char *const[]){ "TZ=Asia/Tokyo", NULL }, out,
        sizeof out);
    CHECK(strcmp(out, "UTC +0000\n") == 0, "date -u: %s", out);
    run((char *const[]){ "/bin/date", "+%Z %z", NULL }, (char *const[]){ "TZ=Asia/Tokyo", NULL }, out, sizeof out);
    CHECK(strcmp(out, "JST +0900\n") == 0, "date in Tokyo: %s", out);
    run((char *const[]){ "/bin/lua", "-e", "print(os.date('%H:%M %Z', 0))", NULL },
        (char *const[]){ "TZ=Europe/Moscow", NULL }, out, sizeof out);
    CHECK(strcmp(out, "03:00 MSK\n") == 0, "Lua os.date in Moscow: %s", out);
}

int main(void)
{
    test_rules();
    test_files();
    test_localtime_file();
    test_programs();
    printf("tztest: %d failures\n", failures);
    return failures != 0;
}
