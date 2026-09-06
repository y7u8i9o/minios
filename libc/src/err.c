#include <err.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void report(int code, int with_code, const char *fmt, va_list ap)
{
    fprintf(stderr, "%s: ", getprogname());
    if (fmt != NULL)
        vfprintf(stderr, fmt, ap);
    if (with_code)
        fprintf(stderr, "%s%s", fmt != NULL ? ": " : "", strerror(code));
    fputc('\n', stderr);
}

void vwarnc(int code, const char *fmt, va_list ap)
{
    report(code, 1, fmt, ap);
}

void vwarn(const char *fmt, va_list ap)
{
    report(errno, 1, fmt, ap);
}

void vwarnx(const char *fmt, va_list ap)
{
    report(0, 0, fmt, ap);
}

void warn(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vwarn(fmt, ap);
    va_end(ap);
}

void warnc(int code, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vwarnc(code, fmt, ap);
    va_end(ap);
}

void warnx(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vwarnx(fmt, ap);
    va_end(ap);
}

void verrc(int status, int code, const char *fmt, va_list ap)
{
    report(code, 1, fmt, ap);
    exit(status);
}

void verr(int status, const char *fmt, va_list ap)
{
    verrc(status, errno, fmt, ap);
}

void verrx(int status, const char *fmt, va_list ap)
{
    report(0, 0, fmt, ap);
    exit(status);
}

void err(int status, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    verr(status, fmt, ap);
}

void errc(int status, int code, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    verrc(status, code, fmt, ap);
}

void errx(int status, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    verrx(status, fmt, ap);
}
