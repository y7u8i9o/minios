/* date: print the current local time, or with -u the time in UTC,
 * optionally with a strftime format (+FORMAT), or set the clock with -s
 * "YYYY-MM-DD HH:MM:SS" in local time.  The default format is date_fmt of
 * LC_TIME. */
#include <langinfo.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* "YYYY-MM-DD[ HH:MM[:SS]]" into the fields. */
static int parse(const char *text, struct tm *tm)
{
    memset(tm, 0, sizeof *tm);
    int fields[6] = { 0, 0, 0, 0, 0, 0 };
    const char *p = text;
    int n = 0;
    for (; n < 6; n++) {
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p)
            break;
        fields[n] = (int)v;
        p = end;
        if (*p == '-' || *p == ':' || *p == ' ' || *p == 'T')
            p++;
    }
    int year = fields[0], mon = fields[1], day = fields[2], hour = fields[3], min = fields[4], sec = fields[5];
    if (n < 3 || *p || mon < 1 || mon > 12 || day < 1 || day > 31 || hour > 23 || min > 59 || sec > 60)
        return -1;
    tm->tm_year = year - 1900;
    tm->tm_mon = mon - 1;
    tm->tm_mday = day;
    tm->tm_hour = hour;
    tm->tm_min = min;
    tm->tm_sec = sec;
    tm->tm_isdst = -1;
    return 0;
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    const char *format = nl_langinfo(_DATE_FMT);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            struct tm tm;
            if (parse(argv[++i], &tm) < 0) {
                fprintf(stderr, "date: expected YYYY-MM-DD HH:MM:SS\n");
                return 1;
            }
            struct timespec ts = { mktime(&tm), 0 };
            if (clock_settime(CLOCK_REALTIME, &ts) < 0) {
                perror("date: clock_settime");
                return 1;
            }
        } else if (strcmp(argv[i], "-u") == 0) {
            setenv("TZ", "UTC0", 1);
        } else if (argv[i][0] == '+') {
            format = argv[i] + 1;
        } else {
            fprintf(stderr, "usage: date [-u] [+FORMAT] [-s \"YYYY-MM-DD HH:MM:SS\"]\n");
            return 1;
        }
    }
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char buf[256];
    strftime(buf, sizeof buf, format, &tm);
    printf("%s\n", buf);
    return 0;
}
