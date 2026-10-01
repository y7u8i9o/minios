/* The long double functions of aarch64 and the primitives that math.c
 * and math_extra.c take from the architecture (stdlib/math_arch.h).
 *
 * long double is IEEE binary128 on aarch64; libgcc implements its
 * arithmetic in software. The exact functions (truncl, frexpl, ldexpl,
 * fmodl, remainderl) work on the binary128 representation. The
 * exponential, logarithmic and inverse tangent functions compute in
 * double precision with the kernels below, so their long double results
 * carry double precision only, and the double functions that math.c
 * builds on them (exp, log, pow, atan) can be one unit off where the x87
 * kernels of x86_64 round correctly. */
#include <math.h>
#include <float.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib/math_arch.h>

union quad_bits {
    long double value;
    struct { uint64_t lo, hi; } words;
};

#define QUAD_SIGN (1ULL << 63)
#define QUAD_EXP(hi) ((unsigned)((hi) >> 48) & 0x7fffU)

/* ---- double kernels ---- */

static const double LN2_HI = 6.93147180369123816490e-01;   /* 0x3fe62e42fee00000 */
static const double LN2_LO = 1.90821492927058770002e-10;
static const double LOG2E = 1.44269504088896338700e+00;
static const double PI = 3.14159265358979311600e+00;
static const double PI_2 = 1.57079632679489655800e+00;
static const double PI_6 = 5.23598775598298815659e-01;
static const double SQRT3 = 1.73205080756887719318e+00;
static const double TAN_PI_12 = 2.67949192431122696e-01;

/* e**t for |t| <= 0.35: the Taylor series to degree 14. */
static double exp_series(double t)
{
    double p = 1.0 / 87178291200.0;                 /* 1/14! */
    static const double inverse_factorial[] = {
        1.0 / 6227020800.0, 1.0 / 479001600.0, 1.0 / 39916800.0, 1.0 / 3628800.0,
        1.0 / 362880.0, 1.0 / 40320.0, 1.0 / 5040.0, 1.0 / 720.0, 1.0 / 120.0,
        1.0 / 24.0, 1.0 / 6.0, 0.5, 1.0, 1.0,
    };
    for (unsigned i = 0; i < sizeof inverse_factorial / sizeof inverse_factorial[0]; i++)
        p = p * t + inverse_factorial[i];
    return p;
}

/* 2**x for finite x within the double range. */
static double exp2_kernel(double x)
{
    double n = floor(x + 0.5);
    double f = x - n;                               /* exact, |f| <= 0.5 */
    double t = f * LN2_HI + f * LN2_LO;
    return ldexp(exp_series(t), (int)n);
}

/* log2 of m in [0.5, 1), returned as the natural logarithm of m scaled to
 * [sqrt(1/2), sqrt(2)) and the exponent adjustment. */
static double log_mantissa(double m, int *adjust)
{
    *adjust = 0;
    if (m < 0.70710678118654752440) {
        m *= 2.0;
        *adjust = -1;
    }
    /* ln(m) = 2 atanh(s), s = (m - 1) / (m + 1), |s| <= 0.172. */
    double s = (m - 1.0) / (m + 1.0);
    double s2 = s * s;
    double p = 2.0 / 25.0;
    for (int k = 23; k >= 1; k -= 2)
        p = p * s2 + 2.0 / k;
    return s * p;
}

/* log2 of a positive finite double. */
static double log2_kernel(double x)
{
    int e;
    double m = frexp(x, &e);
    int adjust;
    double ln = log_mantissa(m, &adjust);
    return (double)(e + adjust) + ln * LOG2E;
}

/* atan(t) for 0 <= t <= 1. */
static double atan_unit(double t)
{
    double base = 0.0;
    if (t > TAN_PI_12) {
        t = (t * SQRT3 - 1.0) / (SQRT3 + t);
        base = PI_6;
    }
    double t2 = t * t;
    double p = 1.0 / 41.0;
    for (int k = 39; k >= 1; k -= 2)
        p = -p * t2 + 1.0 / k;
    return base + t * p;
}

static double atan_kernel(double x)
{
    double a = fabs(x);
    double r = a > 1.0 ? PI_2 - atan_unit(1.0 / a) : atan_unit(a);
    return x < 0.0 ? -r : r;
}

static double atan2_kernel(double y, double x)
{
    if (isnan(x) || isnan(y))
        return x + y;
    if (y == 0.0) {
        if (signbit(x))
            return signbit(y) ? -PI : PI;
        return y;
    }
    if (x == 0.0)
        return y > 0.0 ? PI_2 : -PI_2;
    if (isinf(x)) {
        if (isinf(y))
            return x > 0.0 ? copysign(PI_2 / 2.0, y) : copysign(3.0 * PI_2 / 2.0, y);
        return x > 0.0 ? copysign(0.0, y) : copysign(PI, y);
    }
    if (isinf(y))
        return copysign(PI_2, y);
    double r = atan_kernel(fabs(y / x));
    if (x < 0.0)
        r = PI - r;
    return y < 0.0 ? -r : r;
}

/* ---- primitives of stdlib/math_arch.h ---- */

double sqrt(double x)
{
    if (x < 0.0) {
        errno = EDOM;
        return NAN;
    }
    double result;
    __asm__("fsqrt %d0, %d1" : "=w"(result) : "w"(x));
    return result;
}

float sqrtf(float x)
{
    if (x < 0.0f) {
        errno = EDOM;
        return NAN;
    }
    float result;
    __asm__("fsqrt %s0, %s1" : "=w"(result) : "w"(x));
    return result;
}

/* The remainder of |x| / |y| by exact subtractions of y scaled to the
 * exponent of the rest; *odd receives the parity of the quotient. Every
 * subtraction is exact (Sterbenz), so the result is exact. */
#define PARTIAL_REMAINDER(type, frexp_fn, ldexp_fn)                       \
    {                                                                     \
        type a = x < 0 ? -x : x, b = y < 0 ? -y : y;                      \
        int odd = 0;                                                      \
        if (a >= b) {                                                     \
            int eb;                                                       \
            frexp_fn(b, &eb);                                             \
            while (a >= b) {                                              \
                int ea;                                                   \
                frexp_fn(a, &ea);                                         \
                int shift = ea - eb;                                      \
                type t = ldexp_fn(b, shift);                              \
                if (t > a) {                                              \
                    shift--;                                              \
                    t = ldexp_fn(b, shift);                               \
                }                                                         \
                a -= t;                                                   \
                if (shift == 0)                                           \
                    odd = 1;                                              \
            }                                                             \
        }                                                                 \
        if (nearest && (a > b - a || (a == b - a && odd)))                \
            a -= b;                                                       \
        return signbit(x) ? -a : a;                                       \
    }

double __math_partial_remainder(double x, double y, int nearest)
PARTIAL_REMAINDER(double, frexp, ldexp)

static long double partial_remainder_long(long double x, long double y, int nearest)
PARTIAL_REMAINDER(long double, frexpl, ldexpl)

long double atanl(long double x)
{
    if (isnan(x))
        return x;
    return atan_kernel((double)x);
}

long double atan2l(long double y, long double x)
{
    if (isnan(x))
        return x;
    if (isnan(y))
        return y;
    return atan2_kernel((double)y, (double)x);
}

/* ---- binary128 representation ---- */

long double fabsl(long double x)
{
    union quad_bits u = { x };
    u.words.hi &= ~QUAD_SIGN;
    return u.value;
}

long double copysignl(long double x, long double y)
{
    union quad_bits ux = { x }, uy = { y };
    ux.words.hi = (ux.words.hi & ~QUAD_SIGN) | (uy.words.hi & QUAD_SIGN);
    return ux.value;
}

long double sqrtl(long double x)
{
    if (x < 0.0L) {
        errno = EDOM;
        return NAN;
    }
    if (x == 0.0L || isinf(x) || isnan(x))
        return x;
    int e;
    long double m = frexpl(x, &e);                  /* x = m * 2**e, m in [0.5, 1) */
    if (e & 1) {
        m *= 2.0L;
        e--;
    }
    long double r = sqrt((double)m);
    r = 0.5L * (r + m / r);                         /* one Newton step: double to quad precision */
    r = 0.5L * (r + m / r);
    return ldexpl(r, e / 2);
}

long double truncl(long double x)
{
    union quad_bits u = { x };
    unsigned raw = QUAD_EXP(u.words.hi);
    int exponent = (int)raw - 16383;
    if (raw == 0x7fffU || exponent >= 112)
        return x;
    if (exponent < 0) {
        u.words.hi &= QUAD_SIGN;
        u.words.lo = 0;
        return u.value;
    }
    int fraction_bits = 112 - exponent;
    if (fraction_bits >= 64) {
        u.words.lo = 0;
        u.words.hi &= ~((1ULL << (fraction_bits - 64)) - 1ULL);
    } else {
        u.words.lo &= ~((1ULL << fraction_bits) - 1ULL);
    }
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
    union quad_bits u = { x };
    unsigned raw = QUAD_EXP(u.words.hi);
    *exponent = 0;
    if (raw == 0) {
        if ((u.words.hi & ~QUAD_SIGN) == 0 && u.words.lo == 0)
            return x;
        long double scaled = frexpl(x * 0x1p120L, exponent);
        *exponent -= 120;
        return scaled;
    }
    if (raw == 0x7fffU)
        return x;
    *exponent = (int)raw - 16382;
    u.words.hi = (u.words.hi & ~(0x7fffULL << 48)) | (16382ULL << 48);
    return u.value;
}

/* 2**n for n in the normal exponent range. */
static long double quad_pow2(int n)
{
    union quad_bits u;
    u.words.lo = 0;
    u.words.hi = (uint64_t)(n + 16383) << 48;
    return u.value;
}

long double ldexpl(long double x, int exponent)
{
    if (x == 0.0L || !isfinite(x))
        return x;
    while (exponent > 16383) {
        x *= quad_pow2(16383);
        exponent -= 16383;
        if (isinf(x))
            break;
    }
    while (exponent < -16382) {
        x *= quad_pow2(-16382);
        exponent += 16382;
        if (x == 0.0L)
            break;
    }
    long double result = isfinite(x) && x != 0.0L ? x * quad_pow2(exponent) : x;
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
    return partial_remainder_long(x, y, 0);
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
    return partial_remainder_long(x, y, 1);
}

/* ---- exponentials and logarithms in double precision ---- */

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
    if (x < -16495.0L) {
        errno = ERANGE;
        return 0.0L;
    }
    long double integral = floorl(x);
    return ldexpl(exp2_kernel((double)(x - integral)), (int)integral);
}

long double expl(long double x)
{
    return exp2l(x * M_LOG2EL);
}

long double expm1l(long double x)
{
    if (x == 0.0L || isnan(x))
        return x;
    if (fabsl(x) <= M_LN2L) {
        /* The series without the leading 1, to degree 20. */
        double t = (double)x;
        double p = 1.0 / 2432902008176640000.0;       /* 1/20! */
        double factorial = 2432902008176640000.0;
        for (int k = 19; k >= 1; k--) {
            factorial /= k + 1;
            p = p * t + 1.0 / factorial;
        }
        return t * p;
    }
    return expl(x) - 1.0L;
}

/* log2 of a positive finite long double: the exponent from frexpl, the
 * mantissa through the double kernel. */
static long double log2_positive(long double x)
{
    int e;
    long double m = frexpl(x, &e);
    int adjust;
    double ln = log_mantissa((double)m, &adjust);
    return (long double)(e + adjust) + (long double)ln * M_LOG2EL;
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
    return log2_positive(x) * M_LN2L;
}

long double log2l(long double x)
{
    if (x <= 0.0L || !isfinite(x))
        return logl(x) * M_LOG2EL;
    int e;
    long double m = frexpl(x, &e);
    if (m == 0.5L)
        return (long double)(e - 1);                /* exact for powers of two */
    return (long double)log2_kernel((double)m) + (long double)e;
}

long double log10l(long double x)
{
    if (x <= 0.0L || !isfinite(x))
        return logl(x) * M_LOG10EL;
    return log2_positive(x) * (M_LN2L * M_LOG10EL);
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
        /* log1p(x) = 2 atanh(s), s = x / (2 + x), |s| <= 1/3. */
        double s = (double)(x / (2.0L + x));
        double s2 = s * s;
        double p = 2.0 / 61.0;
        for (int k = 59; k >= 1; k -= 2)
            p = p * s2 + 2.0 / k;
        return s * p;
    }
    return logl(1.0L + x);
}

static int integral_is_odd(long double value)
{
    value = fabsl(value);
    if (value >= 0x1p113L)
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
    long double result = exp2l(y * log2l(magnitude));
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
