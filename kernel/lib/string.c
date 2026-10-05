#include <stdint.h>
#include <lib/string.h>

void *memcpy(void *dst, const void *src, size_t n)
{
    /* Word copies: under TCG a translated loop of 8 byte moves runs far
     * faster than rep movsb, which QEMU emulates one byte at a time. */
    uint8_t *d = dst;
    const uint8_t *s = src;
    if ((((uintptr_t)d ^ (uintptr_t)s) & 7) == 0) {
        while (n && ((uintptr_t)d & 7)) {
            *d++ = *s++;
            n--;
        }
        while (n >= 32) {
            ((uint64_t *)d)[0] = ((const uint64_t *)s)[0];
            ((uint64_t *)d)[1] = ((const uint64_t *)s)[1];
            ((uint64_t *)d)[2] = ((const uint64_t *)s)[2];
            ((uint64_t *)d)[3] = ((const uint64_t *)s)[3];
            d += 32;
            s += 32;
            n -= 32;
        }
        while (n >= 8) {
            *(uint64_t *)d = *(const uint64_t *)s;
            d += 8;
            s += 8;
            n -= 8;
        }
    }
    while (n--)
        *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d == s || n == 0)
        return dst;
    if (d < s || d >= s + n) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    uint64_t v = (uint8_t)c;
    v |= v << 8;
    v |= v << 16;
    v |= v << 32;
    while (n && ((uintptr_t)d & 7)) {
        *d++ = (uint8_t)c;
        n--;
    }
    while (n >= 8) {
        *(uint64_t *)d = v;
        d += 8;
        n -= 8;
    }
    while (n--)
        *d++ = (uint8_t)c;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i])
            return x[i] - y[i];
    }
    return 0;
}

size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

size_t strnlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (uint8_t)*a - (uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    while (n && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0)
        return 0;
    return (uint8_t)*a - (uint8_t)*b;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (*s == '\0')
            return NULL;
    }
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (;; s++) {
        if (*s == (char)c)
            last = s;
        if (*s == '\0')
            return (char *)last;
    }
}

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != '\0')
        ;
    return dst;
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size) {
        size_t n = len < size - 1 ? len : size - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return len;
}

size_t strlcat(char *dst, const char *src, size_t size)
{
    size_t used = strnlen(dst, size);
    if (used == size)
        return size + strlen(src);
    return used + strlcpy(dst + used, src, size - used);
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    for (size_t i = 0; i < n; i++)
        if (p[i] == (unsigned char)c)
            return (void *)(p + i);
    return NULL;
}

char *strstr(const char *haystack, const char *needle)
{
    size_t n = strlen(needle);
    if (n == 0)
        return (char *)haystack;
    for (; *haystack; haystack++)
        if (*haystack == *needle && strncmp(haystack, needle, n) == 0)
            return (char *)haystack;
    return NULL;
}

unsigned long long strtoull(const char *s, char **end, int base)
{
    const char *p = s;
    while (*p == ' ' || *p == '\t')
        p++;
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
        p += 2, base = 16;
    else if (base == 0)
        base = p[0] == '0' ? 8 : 10;
    unsigned long long v = 0;
    bool overflow = false;
    const char *first = p;
    for (;; p++) {
        int d = *p >= '0' && *p <= '9' ? *p - '0'
              : *p >= 'a' && *p <= 'z' ? *p - 'a' + 10
              : *p >= 'A' && *p <= 'Z' ? *p - 'A' + 10 : 99;
        if (d >= base)
            break;
        if (v > (~0ULL - (unsigned)d) / (unsigned)base)
            overflow = true;
        v = v * (unsigned)base + (unsigned)d;
    }
    if (end)
        *end = (char *)(p == first ? s : p);
    return overflow ? ~0ULL : v;
}
