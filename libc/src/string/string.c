#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>

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
    unsigned char *d = dst;
    const unsigned char *s = src;
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
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i])
            return x[i] - y[i];
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = s;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == (unsigned char)c)
            return (void *)(p + i);
    }
    return NULL;
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

char *strcpy(char *dst, const char *src)
{
    char *d = dst;
    while ((*d++ = *src++) != '\0')
        ;
    return dst;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0;
    for (; i < n && src[i]; i++)
        dst[i] = src[i];
    for (; i < n; i++)
        dst[i] = '\0';
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

char *strcat(char *dst, const char *src)
{
    strcpy(dst + strlen(dst), src);
    return dst;
}

char *strncat(char *dst, const char *src, size_t n)
{
    char *d = dst + strlen(dst);
    while (n-- && *src)
        *d++ = *src++;
    *d = '\0';
    return dst;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
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
    return (unsigned char)*a - (unsigned char)*b;
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

char *strstr(const char *h, const char *n)
{
    size_t nl = strlen(n);
    if (nl == 0)
        return (char *)h;
    for (; *h; h++) {
        if (strncmp(h, n, nl) == 0)
            return (char *)h;
    }
    return NULL;
}

size_t strspn(const char *s, const char *accept)
{
    size_t n = 0;
    while (s[n] && strchr(accept, s[n]))
        n++;
    return n;
}

size_t strcspn(const char *s, const char *reject)
{
    size_t n = 0;
    while (s[n] && !strchr(reject, s[n]))
        n++;
    return n;
}

char *strpbrk(const char *s, const char *accept)
{
    for (; *s; s++) {
        if (strchr(accept, *s))
            return (char *)s;
    }
    return NULL;
}

char *strtok_r(char *s, const char *delim, const char **save)
{
    if (!s)
        s = (char *)*save;
    s += strspn(s, delim);
    if (!*s) {
        *save = s;
        return NULL;
    }
    char *tok = s;
    s += strcspn(s, delim);
    if (*s) {
        *s = '\0';
        s++;
    }
    *save = s;
    return tok;
}

char *strtok(char *s, const char *delim)
{
    static const char *save;
    return strtok_r(s, delim, &save);
}

char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

static const char *const errors[] = {
    [0] = "Success",
    [EPERM] = "Operation not permitted",
    [ENOENT] = "No such file or directory",
    [ESRCH] = "No such process",
    [EINTR] = "Interrupted system call",
    [EIO] = "Input/output error",
    [E2BIG] = "Argument list too long",
    [ENOEXEC] = "Exec format error",
    [EBADF] = "Bad file descriptor",
    [ECHILD] = "No child processes",
    [EAGAIN] = "Resource temporarily unavailable",
    [ENOMEM] = "Cannot allocate memory",
    [EACCES] = "Permission denied",
    [EFAULT] = "Bad address",
    [EBUSY] = "Device or resource busy",
    [EEXIST] = "File exists",
    [ENODEV] = "No such device",
    [ENOTDIR] = "Not a directory",
    [EISDIR] = "Is a directory",
    [EINVAL] = "Invalid argument",
    [EMFILE] = "Too many open files",
    [ENOTTY] = "Inappropriate ioctl for device",
    [ENOSPC] = "No space left on device",
    [ESPIPE] = "Illegal seek",
    [EROFS] = "Read-only file system",
    [EPIPE] = "Broken pipe",
    [ERANGE] = "Result out of range",
    [ENAMETOOLONG] = "File name too long",
    [ENOSYS] = "Function not implemented",
    [ENOTEMPTY] = "Directory not empty",
};

char *strerror(int errnum)
{
    if (errnum >= 0 && (size_t)errnum < sizeof errors / sizeof errors[0] && errors[errnum])
        return (char *)errors[errnum];
    return "Unknown error";
}

size_t strlcat(char *dst, const char *src, size_t size)
{
    size_t dl = strnlen(dst, size);
    if (dl == size)
        return size + strlen(src);
    return dl + strlcpy(dst + dl, src, size - dl);
}

/* The sole locale is C, whose collation order is the byte order. */
int strcoll(const char *a, const char *b)
{
    return strcmp(a, b);
}

static int lower(int c)
{
    return (c >= 'A' && c <= 'Z') ? c + 0x20 : c;
}

int strcasecmp(const char *a, const char *b)
{
    while (*a != '\0' && lower((unsigned char)*a) == lower((unsigned char)*b)) {
        a++;
        b++;
    }
    return lower((unsigned char)*a) - lower((unsigned char)*b);
}

int strncasecmp(const char *a, const char *b, size_t n)
{
    for (; n > 0; n--, a++, b++) {
        int d = lower((unsigned char)*a) - lower((unsigned char)*b);
        if (d != 0 || *a == '\0')
            return d;
    }
    return 0;
}

int ffs(int value)
{
    return value == 0 ? 0 : __builtin_ctz((unsigned)value) + 1;
}
