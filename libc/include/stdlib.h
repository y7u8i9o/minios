#pragma once
#include <stddef.h>
#include <sys/types.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
#define RAND_MAX 0x7fffffff
#define MB_CUR_MAX 4

void *malloc(size_t size);
void *calloc(size_t n, size_t size);
void *realloc(void *p, size_t size);
void free(void *p);

int atoi(const char *s);
long atol(const char *s);
double atof(const char *s);
long strtol(const char *s, char **end, int base);
unsigned long strtoul(const char *s, char **end, int base);
double strtod(const char *s, char **end);
float strtof(const char *s, char **end);
size_t mbstowcs(wchar_t *dst, const char *src, size_t len);
size_t wcstombs(char *dst, const wchar_t *src, size_t len);
int abs(int v);
long labs(long v);

__attribute__((noreturn)) void exit(int status);
__attribute__((noreturn)) void _Exit(int status);
__attribute__((noreturn)) void abort(void);
int atexit(void (*fn)(void));
char *getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);
int system(const char *command);
int mkstemp(char *template);
int rand(void);
void srand(unsigned seed);
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));

void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *));

/* random has a period of 2^31 - 2 and returns values in [0, 2^31 - 2]. */
long random(void);
void srandom(unsigned seed);

/* The program name is the last component of argv[0]. */
const char *getprogname(void);
void setprogname(const char *name);

/* Stateful multibyte conversion over the UTF-8 encoding. */
int mblen(const char *s, size_t n);
int mbtowc(wchar_t *out, const char *s, size_t n);
int wctomb(char *s, wchar_t wc);
