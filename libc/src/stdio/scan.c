#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stdarg.h>

/* Formatted input. Every conversion reads through a source with one
 * character of push back, which is all the C scanf grammar needs. */

struct source {
    FILE *f;
    const char *s;
    size_t consumed;
};

static int src_get(struct source *src)
{
    int c;
    if (src->f != NULL) {
        c = fgetc(src->f);
    } else {
        c = (unsigned char)*src->s;
        if (c == 0)
            return EOF;
        src->s++;
    }
    if (c != EOF)
        src->consumed++;
    return c;
}

static void src_unget(struct source *src, int c)
{
    if (c == EOF)
        return;
    if (src->f != NULL)
        ungetc(c, src->f);
    else
        src->s--;
    src->consumed--;
}

static void skip_space(struct source *src)
{
    int c;
    while ((c = src_get(src)) != EOF && isspace(c))
        ;
    src_unget(src, c);
}

enum length { LEN_HH, LEN_H, LEN_NONE, LEN_L, LEN_LL, LEN_J, LEN_Z, LEN_T, LEN_BIG };

static void store_int(va_list *ap, enum length len, int is_unsigned, unsigned long long v)
{
    switch (len) {
    case LEN_HH: *va_arg(*ap, char *) = (char)v; break;
    case LEN_H:  *va_arg(*ap, short *) = (short)v; break;
    case LEN_NONE: *va_arg(*ap, int *) = (int)v; break;
    case LEN_L:  *va_arg(*ap, long *) = (long)v; break;
    case LEN_LL: case LEN_BIG: *va_arg(*ap, long long *) = (long long)v; break;
    case LEN_J:  *va_arg(*ap, intmax_t *) = (intmax_t)v; break;
    case LEN_Z:  *va_arg(*ap, size_t *) = (size_t)v; break;
    case LEN_T:  *va_arg(*ap, ptrdiff_t *) = (ptrdiff_t)v; break;
    }
    (void)is_unsigned;
}

/* Collect the longest prefix of an integer in the given base, at most width
 * characters, into buf. Returns the number of characters collected. */
static size_t collect_int(struct source *src, int base, size_t width, char *buf, size_t size)
{
    size_t n = 0;
    int c = src_get(src);
    if (c == '+' || c == '-') {
        if (n < width && n < size - 1)
            buf[n++] = (char)c;
        c = src_get(src);
    }
    if (base == 0 || base == 16) {
        if (c == '0' && n < width) {
            buf[n++] = '0';
            c = src_get(src);
            if ((c == 'x' || c == 'X') && n < width) {
                buf[n++] = (char)c;
                c = src_get(src);
                base = 16;
            } else if (base == 0) {
                base = 8;
            }
        } else if (base == 0) {
            base = 10;
        }
    }
    while (c != EOF && n < width && n < size - 1) {
        int d;
        if (isdigit(c))
            d = c - '0';
        else if (isalpha(c))
            d = tolower(c) - 'a' + 10;
        else
            break;
        if (d >= base)
            break;
        buf[n++] = (char)c;
        c = src_get(src);
    }
    src_unget(src, c);
    buf[n] = '\0';
    return n;
}

static size_t collect_float(struct source *src, size_t width, char *buf, size_t size)
{
    size_t n = 0;
    int c = src_get(src);
    int seen_digit = 0, seen_point = 0, seen_exp = 0, hex = 0;
    if (c == '+' || c == '-') {
        buf[n++] = (char)c;
        c = src_get(src);
    }
    /* inf, infinity and nan */
    if (c == 'i' || c == 'I' || c == 'n' || c == 'N') {
        while (c != EOF && isalpha(c) && n < width && n < size - 1) {
            buf[n++] = (char)c;
            c = src_get(src);
        }
        src_unget(src, c);
        buf[n] = '\0';
        return n;
    }
    while (c != EOF && n < width && n < size - 1) {
        if (isdigit(c) || (hex && isxdigit(c))) {
            seen_digit = 1;
        } else if (c == '.' && !seen_point && !seen_exp) {
            seen_point = 1;
        } else if ((c == 'x' || c == 'X') && n == (size_t)(1 + (buf[0] == '+' || buf[0] == '-')) && buf[n - 1] == '0') {
            hex = 1;
        } else if (((!hex && (c == 'e' || c == 'E')) || (hex && (c == 'p' || c == 'P'))) && seen_digit && !seen_exp) {
            seen_exp = 1;
            buf[n++] = (char)c;
            c = src_get(src);
            if (c == '+' || c == '-') {
                buf[n++] = (char)c;
                c = src_get(src);
            }
            continue;
        } else {
            break;
        }
        buf[n++] = (char)c;
        c = src_get(src);
    }
    src_unget(src, c);
    buf[n] = '\0';
    return n;
}

/* Parse a scanset after '[' up to and including ']'. Sets table[256]. */
static const char *parse_set(const char *fmt, unsigned char *table)
{
    int negate = 0;
    memset(table, 0, 256);
    if (*fmt == '^') {
        negate = 1;
        fmt++;
    }
    if (*fmt == ']') {
        table[(unsigned char)']'] = 1;
        fmt++;
    }
    while (*fmt != '\0' && *fmt != ']') {
        unsigned char lo = (unsigned char)*fmt++;
        if (*fmt == '-' && fmt[1] != ']' && fmt[1] != '\0') {
            unsigned char hi = (unsigned char)fmt[1];
            for (unsigned c = lo; c <= hi; c++)
                table[c] = 1;
            fmt += 2;
        } else {
            table[lo] = 1;
        }
    }
    if (*fmt == ']')
        fmt++;
    if (negate)
        for (int i = 0; i < 256; i++)
            table[i] = !table[i];
    return fmt;
}

static int scan(struct source *src, const char *fmt, va_list ap)
{
    va_list args;
    va_copy(args, ap);
    int assigned = 0;
    int any_input = 0;
    char buf[128];
    unsigned char set[256];

    for (; *fmt != '\0'; fmt++) {
        if (isspace((unsigned char)*fmt)) {
            skip_space(src);
            continue;
        }
        if (*fmt != '%') {
            int c = src_get(src);
            if (c == EOF)
                goto input_failure;
            any_input = 1;
            if (c != (unsigned char)*fmt) {
                src_unget(src, c);
                goto done;
            }
            continue;
        }
        fmt++;
        if (*fmt == '%') {
            skip_space(src);
            int c = src_get(src);
            if (c == EOF)
                goto input_failure;
            if (c != '%') {
                src_unget(src, c);
                goto done;
            }
            continue;
        }
        int suppress = 0;
        if (*fmt == '*') {
            suppress = 1;
            fmt++;
        }
        size_t width = 0;
        while (isdigit((unsigned char)*fmt))
            width = width * 10 + (size_t)(*fmt++ - '0');
        if (width == 0)
            width = (size_t)-1;
        enum length len = LEN_NONE;
        switch (*fmt) {
        case 'h': len = LEN_H; fmt++; if (*fmt == 'h') { len = LEN_HH; fmt++; } break;
        case 'l': len = LEN_L; fmt++; if (*fmt == 'l') { len = LEN_LL; fmt++; } break;
        case 'j': len = LEN_J; fmt++; break;
        case 'z': len = LEN_Z; fmt++; break;
        case 't': len = LEN_T; fmt++; break;
        case 'L': len = LEN_BIG; fmt++; break;
        default: break;
        }
        int conv = (unsigned char)*fmt;
        if (conv == '\0')
            break;

        if (conv == 'n') {
            if (!suppress)
                store_int(&args, len, 0, (unsigned long long)src->consumed);
            continue;
        }

        if (conv != 'c' && conv != '[')
            skip_space(src);

        switch (conv) {
        case 'd': case 'i': case 'u': case 'o': case 'x': case 'X': case 'p': {
            int base = conv == 'i' ? 0 : conv == 'o' ? 8 : (conv == 'x' || conv == 'X' || conv == 'p') ? 16 : 10;
            int c = src_get(src);
            if (c == EOF)
                goto input_failure;
            any_input = 1;
            src_unget(src, c);
            size_t n = collect_int(src, base, width, buf, sizeof buf);
            size_t digits = n;
            if (n > 0 && (buf[0] == '+' || buf[0] == '-'))
                digits--;
            if (digits == 0)
                goto done;
            char *end;
            unsigned long long v;
            if (conv == 'd' || conv == 'i')
                v = (unsigned long long)strtol(buf, &end, base == 0 ? 0 : base);
            else
                v = strtoul(buf, &end, base);
            if (end == buf)
                goto done;
            if (!suppress) {
                if (conv == 'p')
                    *va_arg(args, void **) = (void *)(uintptr_t)v;
                else
                    store_int(&args, len, conv != 'd' && conv != 'i', v);
                assigned++;
            }
            break;
        }
        case 'a': case 'A': case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': {
            int c = src_get(src);
            if (c == EOF)
                goto input_failure;
            any_input = 1;
            src_unget(src, c);
            size_t n = collect_float(src, width, buf, sizeof buf);
            if (n == 0)
                goto done;
            char *end;
            double v = strtod(buf, &end);
            if (end == buf)
                goto done;
            if (!suppress) {
                if (len == LEN_L)
                    *va_arg(args, double *) = v;
                else if (len == LEN_BIG)
                    *va_arg(args, long double *) = v;
                else
                    *va_arg(args, float *) = (float)v;
                assigned++;
            }
            break;
        }
        case 'c': {
            size_t n = width == (size_t)-1 ? 1 : width;
            char *out = suppress ? NULL : va_arg(args, char *);
            size_t got = 0;
            while (got < n) {
                int c = src_get(src);
                if (c == EOF)
                    break;
                any_input = 1;
                if (out != NULL)
                    out[got] = (char)c;
                got++;
            }
            if (got == 0)
                goto input_failure;
            if (got < n)
                goto done;
            if (!suppress)
                assigned++;
            break;
        }
        case 's': {
            char *out = suppress ? NULL : va_arg(args, char *);
            size_t got = 0;
            int c;
            while (got < width && (c = src_get(src)) != EOF) {
                any_input = 1;
                if (isspace(c)) {
                    src_unget(src, c);
                    break;
                }
                if (out != NULL)
                    out[got] = (char)c;
                got++;
            }
            if (got == 0) {
                if (!any_input)
                    goto input_failure;
                goto done;
            }
            if (out != NULL) {
                out[got] = '\0';
                assigned++;
            }
            break;
        }
        case '[': {
            fmt = parse_set(fmt + 1, set) - 1;
            char *out = suppress ? NULL : va_arg(args, char *);
            size_t got = 0;
            int c;
            while (got < width && (c = src_get(src)) != EOF) {
                any_input = 1;
                if (!set[c]) {
                    src_unget(src, c);
                    break;
                }
                if (out != NULL)
                    out[got] = (char)c;
                got++;
            }
            if (got == 0) {
                if (!any_input)
                    goto input_failure;
                goto done;
            }
            if (out != NULL) {
                out[got] = '\0';
                assigned++;
            }
            break;
        }
        default:
            goto done;
        }
    }
done:
    va_end(args);
    return assigned;
input_failure:
    va_end(args);
    return assigned > 0 ? assigned : EOF;
}

int vfscanf(FILE *f, const char *fmt, va_list ap)
{
    struct source src = { f, NULL, 0 };
    return scan(&src, fmt, ap);
}

int vsscanf(const char *s, const char *fmt, va_list ap)
{
    struct source src = { NULL, s, 0 };
    return scan(&src, fmt, ap);
}

int vscanf(const char *fmt, va_list ap)
{
    return vfscanf(stdin, fmt, ap);
}

int fscanf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vfscanf(f, fmt, ap);
    va_end(ap);
    return r;
}

int sscanf(const char *s, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vsscanf(s, fmt, ap);
    va_end(ap);
    return r;
}

int scanf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = vfscanf(stdin, fmt, ap);
    va_end(ap);
    return r;
}
