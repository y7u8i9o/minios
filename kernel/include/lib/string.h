#pragma once
#include <kernel.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strcpy(char *dst, const char *src);
size_t strlcpy(char *dst, const char *src, size_t size);
/* Append src to the string in dst of size bytes, truncated, and return the
 * length the result would have without truncation. */
size_t strlcat(char *dst, const char *src, size_t size);
void *memchr(const void *s, int c, size_t n);
char *strstr(const char *haystack, const char *needle);
