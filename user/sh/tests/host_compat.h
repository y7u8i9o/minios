#pragma once
#include <string.h>
#include <stdio.h>
extern char **environ;
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
