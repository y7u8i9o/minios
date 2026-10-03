/* The system log in /var/log/messages (syslog.h). */
#include <syslog.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/utsname.h>

/* The settings of openlog and setlogmask, per process. */
static char log_ident[64];
static int log_option;
static int log_facility = LOG_USER;
static int log_mask = 0xff;

void openlog(const char *ident, int option, int facility)
{
    if (ident)
        snprintf(log_ident, sizeof log_ident, "%s", ident);
    log_option = option;
    if (facility)
        log_facility = facility & LOG_FACMASK;
}

void closelog(void)
{
    log_ident[0] = '\0';
    log_option = 0;
}

int setlogmask(int mask)
{
    int old = log_mask;
    if (mask)
        log_mask = mask;
    return old;
}

/* Write all of buf to the file at path, opened for appending. */
static int append(const char *path, int flags, const char *buf, size_t len)
{
    int fd = open(path, O_WRONLY | O_APPEND | O_CLOEXEC | flags, 0640);
    if (fd < 0)
        return -1;
    ssize_t n = write(fd, buf, len);
    close(fd);
    return n == (ssize_t)len ? 0 : -1;
}

void vsyslog(int priority, const char *format, va_list ap)
{
    if (!(LOG_MASK(LOG_PRI(priority)) & log_mask))
        return;
    int saved = errno;
    char text[512];
    /* %m stands for the error text of errno at the call. */
    char fmt[512];
    size_t o = 0;
    for (const char *p = format; *p && o + 1 < sizeof fmt; p++) {
        if (p[0] == '%' && p[1] == 'm') {
            const char *e = strerror(saved);
            for (; *e && o + 1 < sizeof fmt; e++)
                fmt[o++] = *e == '%' ? '?' : *e;
            p++;
            continue;
        }
        fmt[o++] = *p;
    }
    fmt[o] = '\0';
    vsnprintf(text, sizeof text, fmt, ap);

    char stamp[32] = "";
    time_t now = time(NULL);
    struct tm tm;
    if (localtime_r(&now, &tm))
        strftime(stamp, sizeof stamp, "%b %e %H:%M:%S", &tm);
    struct utsname u;
    const char *host = uname(&u) == 0 && u.nodename[0] ? u.nodename : "minios";
    const char *ident = log_ident[0] ? log_ident : getprogname() ? getprogname() : "syslog";
    char line[700];
    int n;
    if (log_option & LOG_PID)
        n = snprintf(line, sizeof line, "%s %s %s[%d]: %s\n", stamp, host, ident, (int)getpid(), text);
    else
        n = snprintf(line, sizeof line, "%s %s %s: %s\n", stamp, host, ident, text);
    if (n < 0)
        return;
    size_t len = (size_t)n < sizeof line ? (size_t)n : sizeof line - 1;
    int lost = append(SYSLOG_PATH, O_CREAT, line, len) < 0;
    if (log_option & LOG_PERROR) {
        fprintf(stderr, "%s: %s\n", ident, text);
    } else if (lost && (log_option & LOG_CONS)) {
        append("/dev/console", 0, line, len);
    }
    errno = saved;
}

void syslog(int priority, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vsyslog(priority, format, ap);
    va_end(ap);
}
