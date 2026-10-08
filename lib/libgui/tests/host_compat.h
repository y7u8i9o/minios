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

/* The sleep and processor calls of the minios unistd.h. glibc declares
 * a getcpu with two arguments in sched.h. The header is included first,
 * and the name is then replaced, as for strlcpy. */
#include <sched.h>
#undef getcpu
#define getcpu gui_host_getcpu
int gui_host_getcpu(void);
int sleep_ms(unsigned long ms);
int nproc(void);

/* glibc declares FNM_CASEFOLD only for _GNU_SOURCE, which would also
 * change other declarations of the host headers. The value is the one of
 * glibc and of the BSDs. */
#if defined(__linux__) && !defined(FNM_CASEFOLD)
#define FNM_CASEFOLD (1 << 4)
#endif
