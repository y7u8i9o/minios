#pragma once

#include <stddef.h>
#include <string.h>

/* Use one implementation on every host. Include the native declaration
 * first, then replace its name: Darwin may define strlcpy as a fortified
 * macro, and defining a function with that macro active breaks compilation. */
#undef strlcpy
#define strlcpy gui_host_strlcpy
size_t gui_host_strlcpy(char *dst, const char *src, size_t size);

/* The clocks of the minios unistd.h, which the host unistd.h lacks. The
 * fake client defines them with gettimeofday. */
long uptime_ms(void);
long uptime_us(void);

/* glibc declares FNM_CASEFOLD only for _GNU_SOURCE, which would also
 * change other declarations of the host headers. The value is the one of
 * glibc and of the BSDs. */
#if defined(__linux__) && !defined(FNM_CASEFOLD)
#define FNM_CASEFOLD (1 << 4)
#endif
