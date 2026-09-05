#pragma once

#include <stddef.h>

/* Available in MiniOS and macOS libc, but not in glibc. */
size_t strlcpy(char *dst, const char *src, size_t size);
