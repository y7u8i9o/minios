#pragma once
#include <stddef.h>

int strcasecmp(const char *a, const char *b);
int strncasecmp(const char *a, const char *b, size_t n);
int ffs(int value);
/* The BSD names of memmove, memset to zero and memcmp. */
#define bcopy(src, dst, n) ((void)__builtin_memmove((dst), (src), (n)))
#define bzero(dst, n) ((void)__builtin_memset((dst), 0, (n)))
#define bcmp(a, b, n) __builtin_memcmp((a), (b), (n))
