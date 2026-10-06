#pragma once
#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
void *memchr(const void *s, int c, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
char *strcpy(char *dst, const char *src);
char *strncpy(char *dst, const char *src, size_t n);
size_t strlcpy(char *dst, const char *src, size_t size);
char *strcat(char *dst, const char *src);
char *strncat(char *dst, const char *src, size_t n);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
/* strcoll and strxfrm follow LC_COLLATE (docs/design/locale.md). */
int strcoll(const char *a, const char *b);
size_t strxfrm(char *dst, const char *src, size_t n);
struct __locale_struct;
int strcoll_l(const char *a, const char *b, struct __locale_struct *loc);
size_t strxfrm_l(char *dst, const char *src, size_t n, struct __locale_struct *loc);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *h, const char *n);
/* The first occurrence of the nl bytes of n in the hl bytes of h. */
void *memmem(const void *h, size_t hl, const void *n, size_t nl);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char *strpbrk(const char *s, const char *accept);
char *strtok(char *s, const char *delim);
char *strtok_r(char *s, const char *delim, char **save);
char *strdup(const char *s);
size_t strlcat(char *dst, const char *src, size_t size);
char *strerror(int errnum);

char *strndup(const char *s, size_t n);
char *stpcpy(char *dst, const char *src);
/* Split *stringp at the first of the characters of delim, as in BSD. */
char *strsep(char **stringp, const char *delim);
/* The BSD functions of strings.h, which BSD and glibc programs expect
 * from string.h as well. */
#include <strings.h>
