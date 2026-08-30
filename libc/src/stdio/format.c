#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* Formatter shared by the printf family. Supports %d %i %u %x %X %o %p %s
 * %c %%, the flags - 0 + space, width and precision (also *), and the
 * length modifiers hh h l ll z t. */
typedef void (*emit_fn)(char c, void *arg);

struct out {
    emit_fn emit;
    void *arg;
    int count;
};

static void out_char(struct out *o, char c)
{
    o->emit(c, o->arg);
    o->count++;
}

static void out_pad(struct out *o, int n, char c)
{
    while (n-- > 0)
        out_char(o, c);
}

int __vformat(emit_fn emit, void *arg, const char *fmt, va_list ap)
{
    struct out o = { emit, arg, 0 };
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            out_char(&o, *fmt);
            continue;
        }
        fmt++;
        int left = 0, zero = 0, plus = 0, space = 0, alt = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '0') zero = 1;
            else if (*fmt == '+') plus = 1;
            else if (*fmt == ' ') space = 1;
            else if (*fmt == '#') alt = 1;
            else break;
        }
        int width = 0;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                left = 1;
                width = -width;
            }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + (*fmt++ - '0');
        }
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9')
                    prec = prec * 10 + (*fmt++ - '0');
            }
        }
        int lmod = 0;   /* -2 hh, -1 h, 0 int, 1 long, 2 long long */
        for (;; fmt++) {
            if (*fmt == 'l') lmod++;
            else if (*fmt == 'h') lmod--;
            else if (*fmt == 'z' || *fmt == 't' || *fmt == 'j') lmod = 1;
            else break;
        }

        char conv = *fmt;
        if (!conv)
            break;
        if (conv == '%') {
            out_char(&o, '%');
            continue;
        }
        if (conv == 'c') {
            char c = (char)va_arg(ap, int);
            if (!left) out_pad(&o, width - 1, ' ');
            out_char(&o, c);
            if (left) out_pad(&o, width - 1, ' ');
            continue;
        }
        if (conv == 's') {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            int len = (int)(prec >= 0 ? strnlen(s, (size_t)prec) : strlen(s));
            if (!left) out_pad(&o, width - len, ' ');
            for (int i = 0; i < len; i++)
                out_char(&o, s[i]);
            if (left) out_pad(&o, width - len, ' ');
            continue;
        }

        /* Integer conversions. */
        int base = 10, upper = 0, is_signed = 0;
        const char *prefix = "";
        unsigned long long v;
        switch (conv) {
        case 'd': case 'i': is_signed = 1; break;
        case 'u': break;
        case 'x': base = 16; if (alt) prefix = "0x"; break;
        case 'X': base = 16; upper = 1; if (alt) prefix = "0X"; break;
        case 'o': base = 8; if (alt) prefix = "0"; break;
        case 'p': base = 16; prefix = "0x"; lmod = 1; break;
        default:
            out_char(&o, '%');
            out_char(&o, conv);
            continue;
        }
        int neg = 0;
        if (is_signed) {
            long long sv = lmod >= 2 ? va_arg(ap, long long) : lmod == 1 ? va_arg(ap, long) : va_arg(ap, int);
            if (lmod == -1) sv = (short)sv;
            if (lmod == -2) sv = (signed char)sv;
            neg = sv < 0;
            v = neg ? (unsigned long long)(-(sv + 1)) + 1 : (unsigned long long)sv;
        } else {
            v = lmod >= 2 ? va_arg(ap, unsigned long long) : lmod == 1 ? va_arg(ap, unsigned long) : va_arg(ap, unsigned);
            if (lmod == -1) v = (unsigned short)v;
            if (lmod == -2) v = (unsigned char)v;
        }
        char digits[32];
        int n = 0;
        const char *set = upper ? "0123456789ABCDEF" : "0123456789abcdef";
        if (!(prec == 0 && v == 0)) {
            do {
                digits[n++] = set[v % (unsigned)base];
                v /= (unsigned)base;
            } while (v);
        }
        int zeros = prec > n ? prec - n : 0;
        const char *sign = neg ? "-" : plus ? "+" : space ? " " : "";
        int total = (int)strlen(sign) + (int)strlen(prefix) + zeros + n;
        int pad = width > total ? width - total : 0;
        if (!left && !(zero && prec < 0))
            out_pad(&o, pad, ' ');
        for (const char *s = sign; *s; s++) out_char(&o, *s);
        for (const char *s = prefix; *s; s++) out_char(&o, *s);
        if (!left && zero && prec < 0)
            out_pad(&o, pad, '0');
        out_pad(&o, zeros, '0');
        while (n)
            out_char(&o, digits[--n]);
        if (left)
            out_pad(&o, pad, ' ');
    }
    return o.count;
}

struct snp {
    char *buf;
    size_t size, pos;
};

static void snp_emit(char c, void *arg)
{
    struct snp *s = arg;
    if (s->pos + 1 < s->size)
        s->buf[s->pos] = c;
    s->pos++;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct snp s = { buf, size, 0 };
    int n = __vformat(snp_emit, &s, fmt, ap);
    if (size)
        buf[s.pos < size ? s.pos : size - 1] = '\0';
    return n;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}

int sprintf(char *buf, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, (size_t)-1, fmt, ap);
    va_end(ap);
    return n;
}
