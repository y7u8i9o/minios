#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <fenv.h>
#include "../ldouble.h"
#include "../locale/locale_impl.h"

/* Formatter shared by the printf family. Supports integer, string and
 * floating conversions, the flags - 0 + space # and ', width and
 * precision. Floating conversions write the radix character of LC_NUMERIC,
 * and the flag ' groups the integer digits of d, i, u, f, F, g and G with
 * its thousands separator. */
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

#define DECIMAL_DIGITS 17
#define FLOAT_BUFFER_SIZE 1152

struct decimal_rep {
    char digit[DECIMAL_DIGITS + 1];
    int exponent;
    int zero;
};

static void decimal_generate(double value, struct decimal_rep *decimal)
{
    long double normalized = (long double)value;
    int exponent = 0;
    memset(decimal->digit, 0, sizeof(decimal->digit));
    if (value == 0.0) {
        decimal->exponent = 0;
        decimal->zero = 1;
        return;
    }
    decimal->zero = 0;
    while (normalized >= 10.0L) {
        normalized /= 10.0L;
        exponent++;
    }
    while (normalized < 1.0L) {
        normalized *= 10.0L;
        exponent--;
    }
    for (int i = 0; i <= DECIMAL_DIGITS; i++) {
        int digit = (int)normalized;
        if (digit < 0)
            digit = 0;
        if (digit > 9)
            digit = 9;
        decimal->digit[i] = (char)digit;
        normalized = (normalized - (long double)digit) * 10.0L;
    }
    decimal->exponent = exponent;
}

/* Round to a count of significant digits. A zero count is useful when a
 * fixed conversion rounds a value such as 0.6 to an integer. */
static void decimal_round(struct decimal_rep *decimal, int keep)
{
    if (decimal->zero || keep < 0) {
        decimal->zero = 1;
        return;
    }
    if (keep > DECIMAL_DIGITS)
        keep = DECIMAL_DIGITS;
    int round_up = decimal->digit[keep] >= 5;
    for (int i = keep; i <= DECIMAL_DIGITS; i++)
        decimal->digit[i] = 0;
    if (!round_up)
        return;
    for (int i = keep - 1; i >= 0; i--) {
        if (++decimal->digit[i] < 10)
            return;
        decimal->digit[i] = 0;
    }
    decimal->digit[0] = 1;
    decimal->exponent++;
    decimal->zero = 0;
}

static void buffer_put(char *buffer, int *length, char c)
{
    if (*length < FLOAT_BUFFER_SIZE - 1)
        buffer[(*length)++] = c;
}

static int decimal_digit_at(const struct decimal_rep *decimal, int position)
{
    if (decimal->zero)
        return 0;
    int index = decimal->exponent - position;
    return index >= 0 && index < DECIMAL_DIGITS ? decimal->digit[index] : 0;
}

static int format_fixed_body(char *buffer, const struct decimal_rep *decimal,
                             int precision, int alternate)
{
    int length = 0;
    int highest = decimal->exponent > 0 ? decimal->exponent : 0;
    for (int position = highest; position >= 0; position--)
        buffer_put(buffer, &length, (char)('0' + decimal_digit_at(decimal, position)));
    if (precision || alternate)
        buffer_put(buffer, &length, '.');
    for (int position = -1; position >= -precision; position--)
        buffer_put(buffer, &length, (char)('0' + decimal_digit_at(decimal, position)));
    buffer[length] = '\0';
    return length;
}

static void format_exponent(char *buffer, int *length, int exponent, int upper)
{
    buffer_put(buffer, length, upper ? 'E' : 'e');
    buffer_put(buffer, length, exponent < 0 ? '-' : '+');
    unsigned magnitude = (unsigned)(exponent < 0 ? -exponent : exponent);
    char digits[8];
    int count = 0;
    do {
        digits[count++] = (char)('0' + magnitude % 10U);
        magnitude /= 10U;
    } while (magnitude);
    while (count < 2)
        digits[count++] = '0';
    while (count)
        buffer_put(buffer, length, digits[--count]);
}

static int format_scientific_body(char *buffer, const struct decimal_rep *decimal,
                                  int precision, int alternate, int upper)
{
    int length = 0;
    buffer_put(buffer, &length, (char)('0' + (decimal->zero ? 0 : decimal->digit[0])));
    if (precision || alternate)
        buffer_put(buffer, &length, '.');
    for (int i = 1; i <= precision; i++)
        buffer_put(buffer, &length,
                   (char)('0' + (decimal->zero || i >= DECIMAL_DIGITS ? 0 : decimal->digit[i])));
    format_exponent(buffer, &length, decimal->zero ? 0 : decimal->exponent, upper);
    buffer[length] = '\0';
    return length;
}

static int trim_general(char *buffer, int length)
{
    int exponent = length;
    int point = -1;
    for (int i = 0; i < length; i++) {
        if (buffer[i] == '.')
            point = i;
        if (buffer[i] == 'e' || buffer[i] == 'E') {
            exponent = i;
            break;
        }
    }
    if (point < 0)
        return length;
    int end = exponent;
    while (end > point + 1 && buffer[end - 1] == '0')
        end--;
    if (end == point + 1)
        end--;
    if (exponent < length) {
        memmove(buffer + end, buffer + exponent, (size_t)(length - exponent));
        length = end + length - exponent;
    } else {
        length = end;
    }
    buffer[length] = '\0';
    return length;
}

static int format_float_body(char *buffer, double value, char conversion,
                             int precision, int alternate)
{
    union { double value; uint64_t bits; } bits = { value };
    int upper = conversion >= 'A' && conversion <= 'Z';
    uint64_t exponent = bits.bits >> 52 & 0x7ff;
    uint64_t fraction = bits.bits & ((1ULL << 52) - 1);
    if (exponent == 0x7ff) {
        const char *special = fraction ? (upper ? "NAN" : "nan") : (upper ? "INF" : "inf");
        memcpy(buffer, special, 4);
        return 3;
    }

    bits.bits &= ~(1ULL << 63);
    value = bits.value;
    struct decimal_rep decimal;
    decimal_generate(value, &decimal);

    char lower = upper ? (char)(conversion - 'A' + 'a') : conversion;
    if (precision < 0)
        precision = 6;
    if (lower == 'g' && precision == 0)
        precision = 1;
    int maximum_precision = lower == 'g' ? DECIMAL_DIGITS : DECIMAL_DIGITS - 1;
    if (precision > maximum_precision)
        precision = maximum_precision;

    if (lower == 'e') {
        decimal_round(&decimal, precision + 1);
        return format_scientific_body(buffer, &decimal, precision, alternate, upper);
    }
    if (lower == 'f') {
        decimal_round(&decimal, decimal.exponent + precision + 1);
        return format_fixed_body(buffer, &decimal, precision, alternate);
    }

    decimal_round(&decimal, precision);
    int length;
    if (decimal.exponent < -4 || decimal.exponent >= precision) {
        length = format_scientific_body(buffer, &decimal, precision - 1, alternate, upper);
    } else {
        int fractional = precision - decimal.exponent - 1;
        length = format_fixed_body(buffer, &decimal, fractional, alternate);
    }
    return alternate ? length : trim_general(buffer, length);
}

struct hex_rep {
    uint64_t significand;
    int exponent;
    int negative;
    int zero;
    int special;
    int exact_digits;
};

static void hex_rep_double(double value, struct hex_rep *hex)
{
    union { double value; uint64_t bits; } u = { value };
    unsigned raw = (unsigned)(u.bits >> 52 & 0x7ffU);
    uint64_t fraction = u.bits & ((1ULL << 52) - 1ULL);
    hex->negative = (int)(u.bits >> 63);
    hex->zero = raw == 0 && fraction == 0;
    hex->special = raw == 0x7ffU ? (fraction ? 2 : 1) : 0;
    hex->exact_digits = 13;
    if (hex->zero || hex->special) {
        hex->significand = 0;
        hex->exponent = 0;
        return;
    }
    if (raw == 0) {
        hex->exponent = -1022;
        while ((fraction & (1ULL << 52)) == 0) {
            fraction <<= 1;
            hex->exponent--;
        }
    } else {
        fraction |= 1ULL << 52;
        hex->exponent = (int)raw - 1023;
    }
    hex->significand = fraction << 11;
}

static void hex_rep_long(long double value, struct hex_rep *hex)
{
    /* On aarch64 (binary128) the significand bits below the leading 64
     * are not printed. */
    struct ld_parts p = ld_split(value);
    unsigned raw = p.raw_exponent;
    hex->negative = (int)p.negative;
    hex->zero = raw == 0 && p.significand == 0 && p.lower == 0;
    hex->special = raw == 0x7fffU ?
        (p.significand == 0x8000000000000000ULL && !p.lower ? 1 : 2) : 0;
    hex->exact_digits = 16;
    if (hex->zero || hex->special) {
        hex->significand = 0;
        hex->exponent = 0;
        return;
    }
    hex->significand = p.significand;
    if (raw == 0) {
        hex->exponent = -16382;
        if (!hex->significand) {
            /* A binary128 subnormal with only lower bits set. */
            hex->significand = p.lower << 15;
            hex->exponent -= 49;
        }
        while ((hex->significand & (1ULL << 63)) == 0) {
            hex->significand <<= 1;
            hex->exponent--;
        }
    } else {
        hex->exponent = (int)raw - 16383;
    }
}

static int hex_fraction_digit(uint64_t significand, int index)
{
    int shift = 59 - index * 4;
    if (shift >= 0)
        return (int)(significand >> shift & 0xfU);
    if (shift > -64)
        return (int)(significand << -shift & 0xfU);
    return 0;
}

static void hex_round(struct hex_rep *hex, int precision)
{
    int keep = 1 + precision * 4;
    if (hex->zero || hex->special || keep >= 64)
        return;
    int shift = 64 - keep;
    uint64_t retained = hex->significand >> shift;
    uint64_t discarded = hex->significand & ((1ULL << shift) - 1ULL);
    if (!discarded)
        return;
    int round_up = 0;
    int mode = fegetround();
    if (mode == FE_UPWARD)
        round_up = !hex->negative;
    else if (mode == FE_DOWNWARD)
        round_up = hex->negative;
    else if (mode == FE_TONEAREST) {
        uint64_t half = 1ULL << (shift - 1);
        round_up = discarded > half || (discarded == half && (retained & 1U));
    }
    if (!round_up)
        return;
    retained++;
    if (retained == (1ULL << keep)) {
        retained >>= 1;
        hex->exponent++;
    }
    hex->significand = retained << shift;
}

static int format_hex_body(char *buffer, struct hex_rep *hex, char conversion,
                           int precision, int alternate)
{
    int upper = conversion == 'A';
    if (hex->special) {
        const char *word = hex->special == 1 ? (upper ? "INF" : "inf") :
                                              (upper ? "NAN" : "nan");
        memcpy(buffer, word, 4);
        return 3;
    }
    int trim = precision < 0;
    if (trim)
        precision = hex->exact_digits;
    if (precision > 1024)
        precision = 1024;
    hex_round(hex, precision);
    if (trim)
        while (precision > 0 && hex_fraction_digit(hex->significand, precision - 1) == 0)
            precision--;

    int length = 0;
    buffer_put(buffer, &length, '0');
    buffer_put(buffer, &length, upper ? 'X' : 'x');
    buffer_put(buffer, &length, hex->zero ? '0' : '1');
    if (precision || alternate)
        buffer_put(buffer, &length, '.');
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    for (int i = 0; i < precision; i++)
        buffer_put(buffer, &length, digits[hex_fraction_digit(hex->significand, i)]);
    buffer_put(buffer, &length, upper ? 'P' : 'p');
    int exponent = hex->zero ? 0 : hex->exponent;
    buffer_put(buffer, &length, exponent < 0 ? '-' : '+');
    unsigned magnitude = (unsigned)(exponent < 0 ? -exponent : exponent);
    char exponent_digits[8];
    int count = 0;
    do {
        exponent_digits[count++] = (char)('0' + magnitude % 10U);
        magnitude /= 10U;
    } while (magnitude);
    while (count)
        buffer_put(buffer, &length, exponent_digits[--count]);
    buffer[length] = '\0';
    return length;
}

/* group_digits writes the n digits of in, most significant first, into
 * out with the thousands separator of LC_NUMERIC between the groups.
 * Returns the length of out, which has room for 4 * n bytes. */
static int group_digits(const char *in, int n, char *out)
{
    const struct locale_data *d = __locale_current()->cat[LC_NUMERIC];
    const char *sep = d->str[THOUSEP], *grouping = d->str[LI_GROUPING];
    size_t seplen = strlen(sep);
    /* Mark the positions, counted from the right, after which a separator
     * follows. */
    char cut[512] = { 0 };
    if (seplen && *grouping) {
        int pos = 0;
        const char *g = grouping;
        int size = *g;
        while (size > 0 && size < 127 && pos + size < n && n <= (int)sizeof cut) {
            pos += size;
            cut[pos] = 1;
            if (g[1])
                size = *++g;
        }
    }
    int len = 0;
    for (int i = 0; i < n; i++) {
        out[len++] = in[i];
        int right = n - 1 - i;
        if (right > 0 && cut[right]) {
            memcpy(out + len, sep, seplen);
            len += (int)seplen;
        }
    }
    return len;
}

/* localize replaces the full stop of a floating body by the radix
 * character and groups the digits before it when group is set. */
static int localize(const char *in, int len, char *out, int group)
{
    const char *radix = __locale_current()->cat[LC_NUMERIC]->str[RADIXCHAR];
    int digits = 0;
    while (digits < len && in[digits] >= '0' && in[digits] <= '9')
        digits++;
    int scientific = 0;
    for (int i = 0; i < len; i++)
        scientific |= in[i] == 'e' || in[i] == 'E';
    int n = 0;
    if (group && !scientific) {
        n = group_digits(in, digits, out);
    } else {
        memcpy(out, in, (size_t)digits);
        n = digits;
    }
    for (int i = digits; i < len; i++) {
        if (in[i] == '.') {
            size_t r = strlen(radix);
            memcpy(out + n, radix, r);
            n += (int)r;
        } else {
            out[n++] = in[i];
        }
    }
    return n;
}

static void out_hex_float(struct out *o, long double value, int is_long,
                          char conversion, int width, int precision, int left,
                          int zero, int plus, int space, int alternate)
{
    struct hex_rep hex;
    if (is_long)
        hex_rep_long(value, &hex);
    else
        hex_rep_double((double)value, &hex);
    char raw[FLOAT_BUFFER_SIZE], body[FLOAT_BUFFER_SIZE + 16];
    int length = format_hex_body(raw, &hex, conversion, precision, alternate);
    length = localize(raw, length, body, 0);
    char sign = hex.negative ? '-' : plus ? '+' : space ? ' ' : '\0';
    int total = length + (sign != '\0');
    int pad = width > total ? width - total : 0;
    if (!left && !zero)
        out_pad(o, pad, ' ');
    if (sign)
        out_char(o, sign);
    if (!left && zero)
        out_pad(o, pad, '0');
    for (int i = 0; i < length; i++)
        out_char(o, body[i]);
    if (left)
        out_pad(o, pad, ' ');
}

static void out_float(struct out *o, double value, char conversion, int width,
                      int precision, int left, int zero, int plus, int space, int alternate, int group)
{
    union { double value; uint64_t bits; } bits = { value };
    char sign = bits.bits >> 63 ? '-' : plus ? '+' : space ? ' ' : '\0';
    char raw[FLOAT_BUFFER_SIZE], body[FLOAT_BUFFER_SIZE * 2];
    int length = format_float_body(raw, value, conversion, precision, alternate);
    length = localize(raw, length, body, group);
    int total = length + (sign != '\0');
    int pad = width > total ? width - total : 0;
    if (!left && !zero)
        out_pad(o, pad, ' ');
    if (sign)
        out_char(o, sign);
    if (!left && zero)
        out_pad(o, pad, '0');
    for (int i = 0; i < length; i++)
        out_char(o, body[i]);
    if (left)
        out_pad(o, pad, ' ');
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
        int left = 0, zero = 0, plus = 0, space = 0, alt = 0, group = 0;
        for (;; fmt++) {
            if (*fmt == '\'') group = 1;
            else if (*fmt == '-') left = 1;
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
            else if (*fmt == 'L') lmod = 3;
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
        if (conv == 'a' || conv == 'A') {
            int is_long = lmod == 3;
            long double value = is_long ? va_arg(ap, long double) :
                                          (long double)va_arg(ap, double);
            out_hex_float(&o, value, is_long, conv, width, prec, left, zero,
                          plus, space, alt);
            continue;
        }
        if (conv == 'f' || conv == 'F' || conv == 'e' || conv == 'E' ||
            conv == 'g' || conv == 'G') {
            double value = lmod == 3 ? (double)va_arg(ap, long double) : va_arg(ap, double);
            out_float(&o, value, conv, width, prec, left, zero, plus, space, alt, group);
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
        if (group && base == 10 && n > 3) {
            char msd[32], grouped[128];
            for (int i = 0; i < n; i++)
                msd[i] = digits[n - 1 - i];
            int g = group_digits(msd, n, grouped);
            int zeros = prec > n ? prec - n : 0;
            const char *sign = neg ? "-" : plus ? "+" : space ? " " : "";
            int total = (int)strlen(sign) + zeros + g;
            int pad = width > total ? width - total : 0;
            if (!left && !(zero && prec < 0))
                out_pad(&o, pad, ' ');
            for (const char *s = sign; *s; s++) out_char(&o, *s);
            if (!left && zero && prec < 0)
                out_pad(&o, pad, '0');
            out_pad(&o, zeros, '0');
            for (int i = 0; i < g; i++)
                out_char(&o, grouped[i]);
            if (left)
                out_pad(&o, pad, ' ');
            continue;
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
