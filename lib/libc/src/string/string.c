#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <bits/simd_types.h>

/* memcpy, memmove and memset move 16 bytes per load and store through the
 * vector type of bits/simd_types.h: SSE2 on x86_64, NEON on aarch64. The
 * loads accept any address. The stores of the main loop go to 16 byte
 * aligned destinations, so the relative alignment of the two regions does
 * not matter. Under TCG a translated loop of vector moves runs far faster
 * than rep movsb, which QEMU emulates one byte at a time.
 *
 * GCC can recognise a copy or fill loop and replace it with a call of
 * memcpy or memset. Inside these functions that call would recurse, so
 * the functions disable the transformation. The fixed size
 * __builtin_memcpy calls below always expand to single moves. */
#define STRING_NO_CALLS __attribute__((optimize("no-tree-loop-distribute-patterns")))

static inline simd_u64x2 load16(const uint8_t *p)
{
    simd_u64x2 v;
    __builtin_memcpy(&v, p, 16);
    return v;
}

static inline void store16(uint8_t *p, simd_u64x2 v)
{
    __builtin_memcpy(p, &v, 16);
}

static inline uint64_t load8(const uint8_t *p)
{
    uint64_t v;
    __builtin_memcpy(&v, p, 8);
    return v;
}

static inline void store8(uint8_t *p, uint64_t v)
{
    __builtin_memcpy(p, &v, 8);
}

static inline uint32_t load4(const uint8_t *p)
{
    uint32_t v;
    __builtin_memcpy(&v, p, 4);
    return v;
}

static inline void store4(uint8_t *p, uint32_t v)
{
    __builtin_memcpy(p, &v, 4);
}

/* Copy fewer than 16 bytes. Every load precedes every store, so the
 * regions may overlap. Two moves of 8 or 4 bytes overlap in the middle
 * when n is not a power of two. */
static inline void copy_small(uint8_t *d, const uint8_t *s, size_t n)
{
    if (n >= 8) {
        uint64_t a = load8(s), b = load8(s + n - 8);
        store8(d, a);
        store8(d + n - 8, b);
    } else if (n >= 4) {
        uint32_t a = load4(s), b = load4(s + n - 4);
        store4(d, a);
        store4(d + n - 4, b);
    } else if (n) {
        uint8_t a = s[0], b = s[n / 2], c = s[n - 1];
        d[0] = a;
        d[n / 2] = b;
        d[n - 1] = c;
    }
}

/* Copy n >= 16 bytes from the start towards the end. The first and the
 * last 16 bytes are loaded before any store and stored after the loop.
 * The loop stores whole aligned blocks of the destination between them.
 * A store never reaches a source byte that a later load reads when the
 * destination lies below the source, so the regions may overlap in that
 * direction. */
STRING_NO_CALLS static void copy_up(uint8_t *d, const uint8_t *s, size_t n)
{
    simd_u64x2 head = load16(s), tail = load16(s + n - 16);
    size_t i = 16 - ((uintptr_t)d & 15);
    for (; i + 64 <= n; i += 64) {
        simd_u64x2 a = load16(s + i), b = load16(s + i + 16), c = load16(s + i + 32), e = load16(s + i + 48);
        store16(d + i, a);
        store16(d + i + 16, b);
        store16(d + i + 32, c);
        store16(d + i + 48, e);
    }
    for (; i + 16 <= n; i += 16)
        store16(d + i, load16(s + i));
    store16(d, head);
    store16(d + n - 16, tail);
}

/* Copy n >= 16 bytes from the end towards the start, for a destination
 * that overlaps the source from above. The loop stores aligned blocks
 * that end at e and moves e down. */
STRING_NO_CALLS static void copy_down(uint8_t *d, const uint8_t *s, size_t n)
{
    simd_u64x2 head = load16(s), tail = load16(s + n - 16);
    size_t e = n - ((uintptr_t)(d + n) & 15);
    for (; e >= 64; e -= 64) {
        simd_u64x2 a = load16(s + e - 16), b = load16(s + e - 32), c = load16(s + e - 48), f = load16(s + e - 64);
        store16(d + e - 16, a);
        store16(d + e - 32, b);
        store16(d + e - 48, c);
        store16(d + e - 64, f);
    }
    for (; e >= 16; e -= 16)
        store16(d + e - 16, load16(s + e - 16));
    store16(d + n - 16, tail);
    store16(d, head);
}

STRING_NO_CALLS void *memcpy(void *restrict dst, const void *restrict src, size_t n)
{
    if (n < 16)
        copy_small(dst, src, n);
    else
        copy_up(dst, src, n);
    return dst;
}

STRING_NO_CALLS void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;
    if (d == s || n == 0)
        return dst;
    if (n < 16)
        copy_small(d, s, n);
    else if (d < s || d >= s + n)
        copy_up(d, s, n);
    else
        copy_down(d, s, n);
    return dst;
}

STRING_NO_CALLS void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = dst;
    uint64_t v = 0x0101010101010101ULL * (uint8_t)c;
    if (n < 16) {
        if (n >= 8) {
            store8(d, v);
            store8(d + n - 8, v);
        } else if (n >= 4) {
            store4(d, (uint32_t)v);
            store4(d + n - 4, (uint32_t)v);
        } else if (n) {
            d[0] = (uint8_t)c;
            d[n / 2] = (uint8_t)c;
            d[n - 1] = (uint8_t)c;
        }
        return dst;
    }
    simd_u64x2 w = { v, v };
    store16(d, w);
    store16(d + n - 16, w);
    size_t i = 16 - ((uintptr_t)d & 15);
    for (; i + 64 <= n; i += 64) {
        store16(d + i, w);
        store16(d + i + 16, w);
        store16(d + i + 32, w);
        store16(d + i + 48, w);
    }
    for (; i + 16 <= n; i += 16)
        store16(d + i, w);
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

char *strtok_r(char *s, const char *delim, char **save)
{
    if (!s)
        s = *save;
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
    static char *save;
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
    [ETXTBSY] = "Text file busy",
    [ENOSPC] = "No space left on device",
    [ESPIPE] = "Illegal seek",
    [EROFS] = "Read-only file system",
    [EPIPE] = "Broken pipe",
    [ERANGE] = "Result out of range",
    [ENAMETOOLONG] = "File name too long",
    [ENOSYS] = "Function not implemented",
    [ENOTEMPTY] = "Directory not empty",
    [ELOOP] = "Too many levels of symbolic links",
    [EOVERFLOW] = "Value too large for defined data type",
    [EBADMSG] = "Bad message",
    [EILSEQ] = "Invalid or incomplete multibyte or wide character",
    [ENOTSOCK] = "Socket operation on non-socket",
    [EDESTADDRREQ] = "Destination address required",
    [EMSGSIZE] = "Message too long",
    [EPROTOTYPE] = "Protocol wrong type for socket",
    [ENOPROTOOPT] = "Protocol not available",
    [EPROTONOSUPPORT] = "Protocol not supported",
    [ESOCKTNOSUPPORT] = "Socket type not supported",
    [EOPNOTSUPP] = "Operation not supported",
    [EAFNOSUPPORT] = "Address family not supported by protocol",
    [EADDRINUSE] = "Address already in use",
    [EADDRNOTAVAIL] = "Cannot assign requested address",
    [ENETDOWN] = "Network is down",
    [ENETUNREACH] = "Network is unreachable",
    [ENETRESET] = "Network dropped connection on reset",
    [ECONNABORTED] = "Software caused connection abort",
    [ECONNRESET] = "Connection reset by peer",
    [ENOBUFS] = "No buffer space available",
    [EISCONN] = "Transport endpoint is already connected",
    [ENOTCONN] = "Transport endpoint is not connected",
    [ESHUTDOWN] = "Cannot send after transport endpoint shutdown",
    [ETIMEDOUT] = "Connection timed out",
    [ECONNREFUSED] = "Connection refused",
    [EHOSTDOWN] = "Host is down",
    [EHOSTUNREACH] = "No route to host",
    [EALREADY] = "Operation already in progress",
    [EINPROGRESS] = "Operation now in progress",
    [ENOMEDIUM] = "No medium found",
    [ECANCELED] = "Operation canceled",
    [EPROTO] = "Protocol error",
    [ESTALE] = "Stale file handle",
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

char *strndup(const char *s, size_t n)
{
    size_t len = strnlen(s, n);
    char *copy = malloc(len + 1);
    if (copy == NULL)
        return NULL;
    memcpy(copy, s, len);
    copy[len] = '\0';
    return copy;
}

char *stpcpy(char *dst, const char *src)
{
    while ((*dst = *src) != '\0') {
        dst++;
        src++;
    }
    return dst;
}

char *strsep(char **stringp, const char *delim)
{
    char *s = *stringp;
    if (!s)
        return NULL;
    char *end = s + strcspn(s, delim);
    if (*end) {
        *end = '\0';
        *stringp = end + 1;
    } else {
        *stringp = NULL;
    }
    return s;
}
