#include <lib/printf.h>
#include <lib/string.h>

#define FLAG_LEFT  (1 << 0)
#define FLAG_ZERO  (1 << 1)

struct out {
    printf_emit_fn emit;
    void *arg;
    int count;
};

static void out_char(struct out *o, char c)
{
    o->emit(c, o->arg);
    o->count++;
}

static void out_padded(struct out *o, const char *s, size_t len, int width, int flags, char pad)
{
    int padn = width > (int)len ? width - (int)len : 0;
    if (!(flags & FLAG_LEFT)) {
        while (padn-- > 0)
            out_char(o, pad);
    }
    for (size_t i = 0; i < len; i++)
        out_char(o, s[i]);
    if (flags & FLAG_LEFT) {
        while (padn-- > 0)
            out_char(o, ' ');
    }
}

static void out_number(struct out *o, uint64_t v, bool negative, unsigned base,
                       bool upper, int width, int flags, const char *prefix)
{
    char tmp[32];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v);

    char buf[48];
    int len = 0;
    if (negative)
        buf[len++] = '-';
    while (*prefix)
        buf[len++] = *prefix++;

    /* Zero padding goes between the sign or prefix and the digits. */
    if ((flags & FLAG_ZERO) && !(flags & FLAG_LEFT)) {
        int total = len + n;
        while (total < width && len < (int)sizeof(buf) - n) {
            buf[len++] = '0';
            total++;
        }
    }
    while (n)
        buf[len++] = tmp[--n];
    out_padded(o, buf, (size_t)len, width, flags, ' ');
}

int kvformat(printf_emit_fn emit, void *arg, const char *fmt, va_list ap)
{
    struct out o = { emit, arg, 0 };

    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            out_char(&o, *fmt);
            continue;
        }
        fmt++;
        int flags = 0;
        for (;; fmt++) {
            if (*fmt == '-')
                flags |= FLAG_LEFT;
            else if (*fmt == '0')
                flags |= FLAG_ZERO;
            else
                break;
        }
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }
        int lcount = 0;
        bool size_t_mod = false;
        while (*fmt == 'l' || *fmt == 'z') {
            if (*fmt == 'l')
                lcount++;
            else
                size_t_mod = true;
            fmt++;
        }

        switch (*fmt) {
        case 'd':
        case 'i': {
            int64_t v;
            if (lcount || size_t_mod)
                v = va_arg(ap, int64_t);
            else
                v = va_arg(ap, int);
            bool neg = v < 0;
            uint64_t mag = neg ? (uint64_t)(-(v + 1)) + 1 : (uint64_t)v;
            out_number(&o, mag, neg, 10, false, width, flags, "");
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            uint64_t v;
            if (lcount || size_t_mod)
                v = va_arg(ap, uint64_t);
            else
                v = va_arg(ap, unsigned int);
            unsigned base = *fmt == 'u' ? 10 : 16;
            out_number(&o, v, false, base, *fmt == 'X', width, flags, "");
            break;
        }
        case 'p': {
            uintptr_t v = (uintptr_t)va_arg(ap, void *);
            out_number(&o, v, false, 16, false, width, flags, "0x");
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s)
                s = "(null)";
            out_padded(&o, s, strlen(s), width, flags, ' ');
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            out_padded(&o, &c, 1, width, flags, ' ');
            break;
        }
        case '%':
            out_char(&o, '%');
            break;
        case '\0':
            return o.count;
        default:
            out_char(&o, '%');
            out_char(&o, *fmt);
            break;
        }
    }
    return o.count;
}

struct snprintf_state {
    char *buf;
    size_t size;
    size_t pos;
};

static void snprintf_emit(char c, void *arg)
{
    struct snprintf_state *st = arg;
    if (st->pos + 1 < st->size)
        st->buf[st->pos] = c;
    st->pos++;
}

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct snprintf_state st = { buf, size, 0 };
    int n = kvformat(snprintf_emit, &st, fmt, ap);
    if (size)
        buf[st.pos < size ? st.pos : size - 1] = '\0';
    return n;
}

int ksnprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = kvsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
