#pragma once
#include <string.h>
#include <stdio.h>
extern char **environ;
/* Darwin's string.h may supply a fortified macro. Make the shell helper
 * private and avoid redefining that macro or a native libc function. */
#undef strlcpy
#define strlcpy sh_host_strlcpy
static inline size_t sh_host_strlcpy(char *dst, const char *src, size_t size)
{
    size_t length = strlen(src);
    if (size) {
        size_t count = length < size - 1 ? length : size - 1;
        memcpy(dst, src, count);
        dst[count] = 0;
    }
    return length;
}
