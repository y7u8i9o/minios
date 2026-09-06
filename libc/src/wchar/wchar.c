#include <wchar.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static mbstate_t internal_state;

static void reset_state(mbstate_t *state)
{
    memset(state, 0, sizeof *state);
}

size_t wcslen(const wchar_t *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

size_t wcsnlen(const wchar_t *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n])
        n++;
    return n;
}

wchar_t *wcscpy(wchar_t *dst, const wchar_t *src)
{
    wchar_t *out = dst;
    while ((*dst++ = *src++) != 0)
        ;
    return out;
}

wchar_t *wcsncpy(wchar_t *dst, const wchar_t *src, size_t n)
{
    size_t i = 0;
    while (i < n && src[i]) {
        dst[i] = src[i];
        i++;
    }
    while (i < n)
        dst[i++] = 0;
    return dst;
}

wchar_t *wcscat(wchar_t *dst, const wchar_t *src)
{
    wcscpy(dst + wcslen(dst), src);
    return dst;
}

wchar_t *wcsncat(wchar_t *dst, const wchar_t *src, size_t n)
{
    wchar_t *at = dst + wcslen(dst);
    size_t i = 0;
    while (i < n && src[i]) {
        at[i] = src[i];
        i++;
    }
    at[i] = 0;
    return dst;
}

int wcscmp(const wchar_t *a, const wchar_t *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a < *b ? -1 : *a != *b;
}

int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (!n)
        return 0;
    return *a < *b ? -1 : *a != *b;
}

int wcscoll(const wchar_t *a, const wchar_t *b)
{
    return wcscmp(a, b);
}

size_t wcsxfrm(wchar_t *dst, const wchar_t *src, size_t n)
{
    size_t length = wcslen(src);
    if (n) {
        size_t copy = length < n - 1 ? length : n - 1;
        wmemcpy(dst, src, copy);
        dst[copy] = 0;
    }
    return length;
}

wchar_t *wcschr(const wchar_t *s, wchar_t c)
{
    for (;; s++) {
        if (*s == c)
            return (wchar_t *)s;
        if (!*s)
            return 0;
    }
}

wchar_t *wcsrchr(const wchar_t *s, wchar_t c)
{
    const wchar_t *last = 0;
    for (;; s++) {
        if (*s == c)
            last = s;
        if (!*s)
            return (wchar_t *)last;
    }
}

wchar_t *wcsstr(const wchar_t *haystack, const wchar_t *needle)
{
    size_t n = wcslen(needle);
    if (!n)
        return (wchar_t *)haystack;
    while (*haystack) {
        if (wcsncmp(haystack, needle, n) == 0)
            return (wchar_t *)haystack;
        haystack++;
    }
    return 0;
}

size_t wcsspn(const wchar_t *s, const wchar_t *accept)
{
    size_t n = 0;
    while (s[n] && wcschr(accept, s[n]))
        n++;
    return n;
}

size_t wcscspn(const wchar_t *s, const wchar_t *reject)
{
    size_t n = 0;
    while (s[n] && !wcschr(reject, s[n]))
        n++;
    return n;
}

wchar_t *wcspbrk(const wchar_t *s, const wchar_t *accept)
{
    while (*s) {
        if (wcschr(accept, *s))
            return (wchar_t *)s;
        s++;
    }
    return 0;
}

wchar_t *wcstok(wchar_t *s, const wchar_t *delim, wchar_t **save)
{
    if (!s)
        s = *save;
    s += wcsspn(s, delim);
    if (!*s) {
        *save = s;
        return 0;
    }
    wchar_t *token = s;
    s += wcscspn(s, delim);
    if (*s)
        *s++ = 0;
    *save = s;
    return token;
}

wchar_t *wcsdup(const wchar_t *s)
{
    size_t size = (wcslen(s) + 1) * sizeof *s;
    wchar_t *copy = malloc(size);
    if (copy)
        memcpy(copy, s, size);
    return copy;
}

wchar_t *wmemcpy(wchar_t *dst, const wchar_t *src, size_t n)
{
    return memcpy(dst, src, n * sizeof *dst);
}

wchar_t *wmemmove(wchar_t *dst, const wchar_t *src, size_t n)
{
    return memmove(dst, src, n * sizeof *dst);
}

wchar_t *wmemset(wchar_t *dst, wchar_t c, size_t n)
{
    for (size_t i = 0; i < n; i++)
        dst[i] = c;
    return dst;
}

int wmemcmp(const wchar_t *a, const wchar_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (s[i] == c)
            return (wchar_t *)(s + i);
    }
    return 0;
}

wint_t btowc(int c)
{
    if (c == EOF)
        return WEOF;
    return (unsigned)c < 0x80 ? (wint_t)(unsigned char)c : WEOF;
}

int wctob(wint_t c)
{
    return c < 0x80 ? (int)c : EOF;
}

int mbsinit(const mbstate_t *state)
{
    return !state || state->__expected == 0;
}

size_t mbrtowc(wchar_t *out, const char *s, size_t n, mbstate_t *state)
{
    if (!state)
        state = &internal_state;
    if (!s) {
        reset_state(state);
        return 0;
    }
    if (!n)
        return (size_t)-2;

    size_t used = 0;
    if (!state->__expected) {
        unsigned char c = (unsigned char)s[used++];
        if (c < 0x80) {
            if (out)
                *out = (wchar_t)c;
            return c ? 1 : 0;
        }
        if (c >= 0xc2 && c <= 0xdf) {
            state->__value = c & 0x1f;
            state->__minimum = 0x80;
            state->__expected = 2;
        } else if (c >= 0xe0 && c <= 0xef) {
            state->__value = c & 0x0f;
            state->__minimum = 0x800;
            state->__expected = 3;
        } else if (c >= 0xf0 && c <= 0xf4) {
            state->__value = c & 0x07;
            state->__minimum = 0x10000;
            state->__expected = 4;
        } else {
            errno = EILSEQ;
            reset_state(state);
            return (size_t)-1;
        }
        state->__seen = 1;
    }

    while (state->__seen < state->__expected && used < n) {
        unsigned char c = (unsigned char)s[used];
        if ((c & 0xc0) != 0x80) {
            errno = EILSEQ;
            reset_state(state);
            return (size_t)-1;
        }
        state->__value = (state->__value << 6) | (c & 0x3f);
        state->__seen++;
        used++;
    }
    if (state->__seen < state->__expected)
        return (size_t)-2;

    uint32_t value = state->__value;
    uint32_t minimum = state->__minimum;
    reset_state(state);
    if (value < minimum || value > 0x10ffff ||
        (value >= 0xd800 && value <= 0xdfff)) {
        errno = EILSEQ;
        return (size_t)-1;
    }
    if (out)
        *out = (wchar_t)value;
    return used;
}

size_t mbrlen(const char *s, size_t n, mbstate_t *state)
{
    return mbrtowc(0, s, n, state);
}

size_t wcrtomb(char *s, wchar_t wc, mbstate_t *state)
{
    if (!state)
        state = &internal_state;
    reset_state(state);
    if (!s)
        return 1;

    uint32_t c = (uint32_t)wc;
    if (c <= 0x7f) {
        s[0] = (char)c;
        return 1;
    }
    if (c <= 0x7ff) {
        s[0] = (char)(0xc0 | (c >> 6));
        s[1] = (char)(0x80 | (c & 0x3f));
        return 2;
    }
    if (c >= 0xd800 && c <= 0xdfff) {
        errno = EILSEQ;
        return (size_t)-1;
    }
    if (c <= 0xffff) {
        s[0] = (char)(0xe0 | (c >> 12));
        s[1] = (char)(0x80 | ((c >> 6) & 0x3f));
        s[2] = (char)(0x80 | (c & 0x3f));
        return 3;
    }
    if (c <= 0x10ffff) {
        s[0] = (char)(0xf0 | (c >> 18));
        s[1] = (char)(0x80 | ((c >> 12) & 0x3f));
        s[2] = (char)(0x80 | ((c >> 6) & 0x3f));
        s[3] = (char)(0x80 | (c & 0x3f));
        return 4;
    }
    errno = EILSEQ;
    return (size_t)-1;
}

size_t mbsrtowcs(wchar_t *dst, const char **src, size_t len, mbstate_t *state)
{
    mbstate_t local = state ? *state : internal_state;
    const char *s = *src;
    size_t count = 0;
    for (;;) {
        wchar_t wc;
        size_t n = mbrtowc(&wc, s, 4, &local);
        if (n == (size_t)-1)
            return (size_t)-1;
        if (n == (size_t)-2) {
            errno = EILSEQ;
            return (size_t)-1;
        }
        if (!dst) {
            if (!wc)
                return count;
        } else {
            if (count == len) {
                *src = s;
                if (state)
                    *state = local;
                else
                    internal_state = local;
                return count;
            }
            dst[count] = wc;
            if (!wc) {
                *src = 0;
                if (state)
                    *state = local;
                else
                    internal_state = local;
                return count;
            }
        }
        count++;
        s += n;
    }
}

size_t wcsrtombs(char *dst, const wchar_t **src, size_t len, mbstate_t *state)
{
    mbstate_t local = state ? *state : internal_state;
    const wchar_t *s = *src;
    size_t count = 0;
    for (;;) {
        char bytes[4];
        size_t n = wcrtomb(bytes, *s, &local);
        if (n == (size_t)-1)
            return (size_t)-1;
        if (!dst) {
            if (!*s)
                return count;
        } else {
            if (n > len - count) {
                *src = s;
                if (state)
                    *state = local;
                else
                    internal_state = local;
                return count;
            }
            memcpy(dst + count, bytes, n);
            if (!*s) {
                *src = 0;
                if (state)
                    *state = local;
                else
                    internal_state = local;
                return count;
            }
        }
        count += n;
        s++;
    }
}

size_t mbstowcs(wchar_t *dst, const char *src, size_t len)
{
    mbstate_t state = { 0 };
    return mbsrtowcs(dst, &src, len, &state);
}

size_t wcstombs(char *dst, const wchar_t *src, size_t len)
{
    mbstate_t state = { 0 };
    return wcsrtombs(dst, &src, len, &state);
}

static char *narrow_number(const wchar_t *s, size_t *length)
{
    size_t n = wcslen(s);
    char *out = malloc(n + 1);
    if (!out) {
        errno = ENOMEM;
        return 0;
    }
    size_t i = 0;
    while (i < n && (uint32_t)s[i] < 0x80) {
        out[i] = (char)s[i];
        i++;
    }
    out[i] = 0;
    *length = i;
    return out;
}

long wcstol(const wchar_t *s, wchar_t **end, int base)
{
    size_t n;
    char *text = narrow_number(s, &n);
    if (!text) {
        if (end)
            *end = (wchar_t *)s;
        return 0;
    }
    char *parsed;
    long value = strtol(text, &parsed, base);
    if (end)
        *end = (wchar_t *)s + (parsed - text);
    free(text);
    return value;
}

unsigned long wcstoul(const wchar_t *s, wchar_t **end, int base)
{
    size_t n;
    char *text = narrow_number(s, &n);
    if (!text) {
        if (end)
            *end = (wchar_t *)s;
        return 0;
    }
    char *parsed;
    unsigned long value = strtoul(text, &parsed, base);
    if (end)
        *end = (wchar_t *)s + (parsed - text);
    free(text);
    return value;
}

double wcstod(const wchar_t *s, wchar_t **end)
{
    size_t n;
    char *text = narrow_number(s, &n);
    if (!text) {
        if (end)
            *end = (wchar_t *)s;
        return 0;
    }
    char *parsed;
    double value = strtod(text, &parsed);
    if (end)
        *end = (wchar_t *)s + (parsed - text);
    free(text);
    return value;
}

float wcstof(const wchar_t *s, wchar_t **end)
{
    size_t n;
    char *text = narrow_number(s, &n);
    if (!text) {
        if (end)
            *end = (wchar_t *)s;
        return 0;
    }
    char *parsed;
    float value = strtof(text, &parsed);
    if (end)
        *end = (wchar_t *)s + (parsed - text);
    free(text);
    return value;
}

wint_t fputwc(wchar_t wc, FILE *stream)
{
    char bytes[4];
    mbstate_t state = { 0 };
    size_t n = wcrtomb(bytes, wc, &state);
    if (n == (size_t)-1 || fwrite(bytes, 1, n, stream) != n)
        return WEOF;
    return (wint_t)wc;
}

wint_t putwc(wchar_t wc, FILE *stream)
{
    return fputwc(wc, stream);
}

wint_t putwchar(wchar_t wc)
{
    return fputwc(wc, stdout);
}

wint_t fgetwc(FILE *stream)
{
    mbstate_t state = { 0 };
    wchar_t wc;
    for (;;) {
        int c = fgetc(stream);
        if (c == EOF) {
            if (!mbsinit(&state))
                errno = EILSEQ;
            return WEOF;
        }
        char byte = (char)c;
        size_t n = mbrtowc(&wc, &byte, 1, &state);
        if (n == (size_t)-1)
            return WEOF;
        if (n != (size_t)-2)
            return (wint_t)wc;
    }
}

wint_t getwc(FILE *stream)
{
    return fgetwc(stream);
}

wint_t getwchar(void)
{
    return fgetwc(stdin);
}

int fputws(const wchar_t *s, FILE *stream)
{
    while (*s) {
        if (fputwc(*s++, stream) == WEOF)
            return -1;
    }
    return 0;
}

wchar_t *fgetws(wchar_t *s, int n, FILE *stream)
{
    if (n <= 0)
        return 0;
    int i = 0;
    while (i + 1 < n) {
        wint_t c = fgetwc(stream);
        if (c == WEOF) {
            if (!i)
                return 0;
            break;
        }
        s[i++] = (wchar_t)c;
        if (c == '\n')
            break;
    }
    s[i] = 0;
    return s;
}

/* mbtowc, wctomb and mblen keep no state between calls because UTF-8 is
 * stateless; a null s resets nothing and reports that. */
int mbtowc(wchar_t *out, const char *s, size_t n)
{
    if (s == NULL)
        return 0;
    if (n == 0)
        return -1;
    if (*s == '\0') {
        if (out != NULL)
            *out = 0;
        return 0;
    }
    mbstate_t state = { 0 };
    size_t r = mbrtowc(out, s, n, &state);
    if (r == (size_t)-1 || r == (size_t)-2)
        return -1;
    return (int)r;
}

int wctomb(char *s, wchar_t wc)
{
    if (s == NULL)
        return 0;
    mbstate_t state = { 0 };
    size_t r = wcrtomb(s, wc, &state);
    return r == (size_t)-1 ? -1 : (int)r;
}

int mblen(const char *s, size_t n)
{
    return mbtowc(NULL, s, n);
}
