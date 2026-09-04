#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef unsigned int wint_t;
typedef struct {
    uint32_t __value;
    uint32_t __minimum;
    unsigned char __expected;
    unsigned char __seen;
} mbstate_t;

#define WEOF ((wint_t)-1)

size_t wcslen(const wchar_t *s);
size_t wcsnlen(const wchar_t *s, size_t max);
wchar_t *wcscpy(wchar_t *dst, const wchar_t *src);
wchar_t *wcsncpy(wchar_t *dst, const wchar_t *src, size_t n);
wchar_t *wcscat(wchar_t *dst, const wchar_t *src);
wchar_t *wcsncat(wchar_t *dst, const wchar_t *src, size_t n);
int wcscmp(const wchar_t *a, const wchar_t *b);
int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n);
int wcscoll(const wchar_t *a, const wchar_t *b);
size_t wcsxfrm(wchar_t *dst, const wchar_t *src, size_t n);
wchar_t *wcschr(const wchar_t *s, wchar_t c);
wchar_t *wcsrchr(const wchar_t *s, wchar_t c);
wchar_t *wcsstr(const wchar_t *haystack, const wchar_t *needle);
size_t wcsspn(const wchar_t *s, const wchar_t *accept);
size_t wcscspn(const wchar_t *s, const wchar_t *reject);
wchar_t *wcspbrk(const wchar_t *s, const wchar_t *accept);
wchar_t *wcstok(wchar_t *s, const wchar_t *delim, wchar_t **save);
wchar_t *wcsdup(const wchar_t *s);

wchar_t *wmemcpy(wchar_t *dst, const wchar_t *src, size_t n);
wchar_t *wmemmove(wchar_t *dst, const wchar_t *src, size_t n);
wchar_t *wmemset(wchar_t *dst, wchar_t c, size_t n);
int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n);
wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n);

wint_t btowc(int c);
int wctob(wint_t c);
int mbsinit(const mbstate_t *state);
size_t mbrlen(const char *s, size_t n, mbstate_t *state);
size_t mbrtowc(wchar_t *out, const char *s, size_t n, mbstate_t *state);
size_t wcrtomb(char *s, wchar_t wc, mbstate_t *state);
size_t mbsrtowcs(wchar_t *dst, const char **src, size_t len, mbstate_t *state);
size_t wcsrtombs(char *dst, const wchar_t **src, size_t len, mbstate_t *state);

long wcstol(const wchar_t *s, wchar_t **end, int base);
unsigned long wcstoul(const wchar_t *s, wchar_t **end, int base);
double wcstod(const wchar_t *s, wchar_t **end);
float wcstof(const wchar_t *s, wchar_t **end);

wint_t fputwc(wchar_t wc, FILE *stream);
wint_t putwc(wchar_t wc, FILE *stream);
wint_t putwchar(wchar_t wc);
wint_t fgetwc(FILE *stream);
wint_t getwc(FILE *stream);
wint_t getwchar(void);
int fputws(const wchar_t *s, FILE *stream);
wchar_t *fgetws(wchar_t *s, int n, FILE *stream);
