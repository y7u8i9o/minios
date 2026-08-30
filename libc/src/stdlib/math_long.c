#include <math.h>
#include <float.h>
#include <errno.h>
#include <stdint.h>

union long_bits {
    long double value;
    struct {
        uint64_t significand;
        uint16_t sign_exponent;
        uint16_t padding[3];
    } parts;
};

long double fabsl(long double x)
{
    union long_bits u = { x };
    u.parts.sign_exponent &= 0x7fffU;
    return u.value;
}

long double copysignl(long double x, long double y)
{
    union long_bits ux = { x }, uy = { y };
    ux.parts.sign_exponent = (uint16_t)((ux.parts.sign_exponent & 0x7fffU) |
                                        (uy.parts.sign_exponent & 0x8000U));
    return ux.value;
}

long double sqrtl(long double x)
{
    if (x < 0.0L) {
        errno = EDOM;
        return NAN;
    }
    long double result;
    __asm__ volatile("fldt %1; fsqrt; fstpt %0"
                     : "=m" (result) : "m" (x) : "st");
    return result;
}

long double truncl(long double x)
{
    union long_bits u = { x };
    unsigned raw = u.parts.sign_exponent & 0x7fffU;
    int exponent = (int)raw - 16383;
    if (raw == 0x7fffU || exponent >= 63)
        return x;
    if (exponent < 0) {
        u.parts.sign_exponent &= 0x8000U;
        u.parts.significand = 0;
        return u.value;
    }
    u.parts.significand &= ~((1ULL << (63 - exponent)) - 1ULL);
    return u.value;
}

long double floorl(long double x)
{
    long double whole = truncl(x);
    return x < whole ? whole - 1.0L : whole;
}

long double ceill(long double x)
{
    long double whole = truncl(x);
    return x > whole ? whole + 1.0L : whole;
}

long double roundl(long double x)
{
    if (x == 0.0L || !isfinite(x))
        return x;
    return x < 0.0L ? ceill(x - 0.5L) : floorl(x + 0.5L);
}

long double fminl(long double x, long double y)
{
    if (isnan(x)) return y;
    if (isnan(y)) return x;
    if (x == y && x == 0.0L)
        return signbit(x) ? x : y;
    return x < y ? x : y;
}

long double fmaxl(long double x, long double y)
{
    if (isnan(x)) return y;
    if (isnan(y)) return x;
    if (x == y && x == 0.0L)
        return signbit(x) ? y : x;
    return x > y ? x : y;
}

long double frexpl(long double x, int *exponent)
{
    union long_bits u = { x };
    unsigned raw = u.parts.sign_exponent & 0x7fffU;
    *exponent = 0;
    if (raw == 0) {
        if (u.parts.significand == 0)
            return x;
        long double scaled = x * 0x1p64L;
        scaled = frexpl(scaled, exponent);
        *exponent -= 64;
        return scaled;
    }
    if (raw == 0x7fffU)
        return x;
    *exponent = (int)raw - 16382;
    u.parts.sign_exponent = (uint16_t)((u.parts.sign_exponent & 0x8000U) | 16382U);
    return u.value;
}

long double ldexpl(long double x, int exponent)
{
    if (x == 0.0L || !isfinite(x))
        return x;
    long double result;
    __asm__ volatile(
        "fildl %[exponent]\n\t"
        "fldt %[value]\n\t"
        "fscale\n\t"
        "fstpt %[result]\n\t"
        "fstp %%st(0)"
        : [result] "=m" (result)
        : [value] "m" (x), [exponent] "m" (exponent)
        : "st", "st(1)");
    if (!isfinite(result) || result == 0.0L)
        errno = ERANGE;
    return result;
}

long double scalbnl(long double x, int exponent)
{
    return ldexpl(x, exponent);
}

long double modfl(long double x, long double *integer)
{
    if (x == 0.0L || isinf(x)) {
        *integer = x;
        return copysignl(0.0L, x);
    }
    if (isnan(x)) {
        *integer = x;
        return x;
    }
    *integer = truncl(x);
    return x - *integer;
}

static long double partial_remainder(long double x, long double y, int nearest)
{
    long double result;
    unsigned short status;
    if (nearest) {
        __asm__ volatile(
            "fldt %[divisor]\n\t"
            "fldt %[value]\n\t"
            "1: fprem1\n\t"
            "fnstsw %%ax\n\t"
            "testw $0x400, %%ax\n\t"
            "jnz 1b\n\t"
            "fstpt %[result]\n\t"
            "fstp %%st(0)"
            : [result] "=m" (result), "=&a" (status)
            : [value] "m" (x), [divisor] "m" (y)
            : "cc", "st", "st(1)");
    } else {
        __asm__ volatile(
            "fldt %[divisor]\n\t"
            "fldt %[value]\n\t"
            "1: fprem\n\t"
            "fnstsw %%ax\n\t"
            "testw $0x400, %%ax\n\t"
            "jnz 1b\n\t"
            "fstpt %[result]\n\t"
            "fstp %%st(0)"
            : [result] "=m" (result), "=&a" (status)
            : [value] "m" (x), [divisor] "m" (y)
            : "cc", "st", "st(1)");
    }
    (void)status;
    return result;
}

long double fmodl(long double x, long double y)
{
    if (isnan(x)) return x;
    if (isnan(y)) return y;
    if (isinf(x) || y == 0.0L) {
        errno = EDOM;
        return NAN;
    }
    if (isinf(y) || x == 0.0L)
        return x;
    return partial_remainder(x, y, 0);
}

long double remainderl(long double x, long double y)
{
    if (isnan(x)) return x;
    if (isnan(y)) return y;
    if (isinf(x) || y == 0.0L) {
        errno = EDOM;
        return NAN;
    }
    if (isinf(y) || x == 0.0L)
        return x;
    return partial_remainder(x, y, 1);
}

static long double exp2_fraction(long double fraction)
{
    long double result;
    __asm__ volatile(
        "fldt %[fraction]\n\t"
        "f2xm1\n\t"
        "fld1\n\t"
        "faddp\n\t"
        "fstpt %[result]"
        : [result] "=m" (result)
        : [fraction] "m" (fraction)
        : "st", "st(1)");
    return result;
}

long double exp2l(long double x)
{
    if (isnan(x) || x == INFINITY)
        return x;
    if (x == -INFINITY)
        return 0.0L;
    if (x >= 16384.0L) {
        errno = ERANGE;
        return HUGE_VALL;
    }
    if (x < -16445.0L) {
        errno = ERANGE;
        return 0.0L;
    }
    long double integral = floorl(x);
    int exponent = (int)integral;
    return ldexpl(exp2_fraction(x - integral), exponent);
}

long double expl(long double x)
{
    return exp2l(x * M_LOG2EL);
}

long double expm1l(long double x)
{
    if (x == 0.0L)
        return x;
    if (fabsl(x) <= M_LN2L) {
        long double scaled = x * M_LOG2EL;
        long double result;
        __asm__ volatile("fldt %1; f2xm1; fstpt %0"
                         : "=m" (result) : "m" (scaled) : "st");
        return result;
    }
    return expl(x) - 1.0L;
}

static long double logarithm_x87(long double x, int kind)
{
    long double result;
    if (kind == 2) {
        __asm__ volatile("fld1; fldt %1; fyl2x; fstpt %0"
                         : "=m" (result) : "m" (x) : "st", "st(1)");
    } else if (kind == 10) {
        __asm__ volatile("fldlg2; fldt %1; fyl2x; fstpt %0"
                         : "=m" (result) : "m" (x) : "st", "st(1)");
    } else {
        __asm__ volatile("fldln2; fldt %1; fyl2x; fstpt %0"
                         : "=m" (result) : "m" (x) : "st", "st(1)");
    }
    return result;
}

long double logl(long double x)
{
    if (isnan(x) || x == INFINITY)
        return x;
    if (x == 0.0L) {
        errno = ERANGE;
        return -HUGE_VALL;
    }
    if (x < 0.0L) {
        errno = EDOM;
        return NAN;
    }
    return logarithm_x87(x, 0);
}

long double log2l(long double x)
{
    if (x <= 0.0L || !isfinite(x))
        return logl(x) * M_LOG2EL;
    return logarithm_x87(x, 2);
}

long double log10l(long double x)
{
    if (x <= 0.0L || !isfinite(x))
        return logl(x) * M_LOG10EL;
    return logarithm_x87(x, 10);
}

long double log1pl(long double x)
{
    if (x == 0.0L || isnan(x) || x == INFINITY)
        return x;
    if (x == -1.0L) {
        errno = ERANGE;
        return -HUGE_VALL;
    }
    if (x < -1.0L) {
        errno = EDOM;
        return NAN;
    }
    if (fabsl(x) <= 0.5L) {
        long double result;
        __asm__ volatile("fldln2; fldt %1; fyl2xp1; fstpt %0"
                         : "=m" (result) : "m" (x) : "st", "st(1)");
        return result;
    }
    return logl(1.0L + x);
}

static int integral_is_odd(long double value)
{
    value = fabsl(value);
    if (value >= 0x1p64L)
        return 0;
    return fmodl(value, 2.0L) == 1.0L;
}

long double powl(long double x, long double y)
{
    if (y == 0.0L || x == 1.0L)
        return 1.0L;
    if (isnan(x) || isnan(y))
        return NAN;
    long double magnitude = fabsl(x);
    if (isinf(y)) {
        if (magnitude == 1.0L)
            return 1.0L;
        if (y > 0.0L)
            return magnitude > 1.0L ? HUGE_VALL : 0.0L;
        return magnitude > 1.0L ? 0.0L : HUGE_VALL;
    }
    int odd = integral_is_odd(y);
    if (x == 0.0L) {
        if (y < 0.0L) {
            errno = ERANGE;
            return signbit(x) && odd ? -HUGE_VALL : HUGE_VALL;
        }
        return signbit(x) && odd ? -0.0L : 0.0L;
    }
    if (x < 0.0L && truncl(y) != y) {
        errno = EDOM;
        return NAN;
    }
    long double result = expl(y * logl(magnitude));
    return x < 0.0L && odd ? -result : result;
}

long double hypotl(long double x, long double y)
{
    x = fabsl(x);
    y = fabsl(y);
    if (isinf(x) || isinf(y))
        return HUGE_VALL;
    if (isnan(x) || isnan(y))
        return NAN;
    if (x < y) {
        long double temporary = x;
        x = y;
        y = temporary;
    }
    if (x == 0.0L)
        return 0.0L;
    long double ratio = y / x;
    long double result = x * sqrtl(1.0L + ratio * ratio);
    if (isinf(result))
        errno = ERANGE;
    return result;
}

long double cbrtl(long double x)
{
    if (x == 0.0L || !isfinite(x))
        return x;
    long double magnitude = fabsl(x);
    long double estimate = expl(logl(magnitude) / 3.0L);
    for (int i = 0; i < 6; i++)
        estimate = (2.0L * estimate + magnitude / (estimate * estimate)) / 3.0L;
    return copysignl(estimate, x);
}

long double nanl(const char *tag)
{
    (void)tag;
    return __builtin_nanl("");
}
