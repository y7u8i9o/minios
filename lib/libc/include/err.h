#pragma once
#include <stdarg.h>

/* BSD error reporting: the program name, a colon, the formatted message
 * and, for err and warn, the text of errno. The err family exits with the
 * given status. */
__attribute__((noreturn, format(printf, 2, 3))) void err(int status, const char *fmt, ...);
__attribute__((noreturn, format(printf, 3, 4))) void errc(int status, int code, const char *fmt, ...);
__attribute__((noreturn, format(printf, 2, 3))) void errx(int status, const char *fmt, ...);
__attribute__((noreturn)) void verr(int status, const char *fmt, va_list ap);
__attribute__((noreturn)) void verrc(int status, int code, const char *fmt, va_list ap);
__attribute__((noreturn)) void verrx(int status, const char *fmt, va_list ap);
__attribute__((format(printf, 1, 2))) void warn(const char *fmt, ...);
__attribute__((format(printf, 2, 3))) void warnc(int code, const char *fmt, ...);
__attribute__((format(printf, 1, 2))) void warnx(const char *fmt, ...);
void vwarn(const char *fmt, va_list ap);
void vwarnc(int code, const char *fmt, va_list ap);
void vwarnx(const char *fmt, va_list ap);
