/* The long double functions of aarch64 and the primitives that math.c
 * and math_extra.c take from the architecture (stdlib/math_arch.h).
 *
 * long double is IEEE binary128 on aarch64; libgcc implements its
 * arithmetic in software. The exact functions (truncl, frexpl, ldexpl,
 * fmodl, remainderl) work on the binary128 representation. The
 * exponential, logarithmic and inverse tangent functions evaluate their
 * series in binary128 after an argument reduction, to within a few units
 * of the last place of binary128; the double functions that math.c and
 * math_extra.c build on them (pow, atan, the hyperbolic functions) round
 * from that result. */
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

/* ---- binary128 kernels ---- */

/* ln 2 split so that n * LN2_HI is exact for the exponents of binary128:
 * LN2_HI has 93 significant bits. */
static const long double LN2_HI = 0x162e42fefa39ef35793c7673p-93L;
static const long double LN2_LO = 1.9470450923807499515879595733332738027853e-31L;
static const long double SQRT3 = 1.7320508075688772935274463415058723669428e+0L;
static const long double PI_2 = 1.5707963267948966192313216916397514420986e+0L;
static const long double PI_6 = 5.2359877559829887307710723054658381403286e-1L;
static const long double TAN_PI_12 = 2.6794919243112270647255365849412763305719e-1L;

/* 1/k! for k = 0 to 30. */
static const long double INV_FACTORIAL[] = {
    1.0000000000000000000000000000000000000000e+0L, 1.0000000000000000000000000000000000000000e+0L,
    5.0000000000000000000000000000000000000000e-1L, 1.6666666666666666666666666666666666666667e-1L,
    4.1666666666666666666666666666666666666667e-2L, 8.3333333333333333333333333333333333333333e-3L,
    1.3888888888888888888888888888888888888889e-3L, 1.9841269841269841269841269841269841269841e-4L,
    2.4801587301587301587301587301587301587302e-5L, 2.7557319223985890652557319223985890652557e-6L,
    2.7557319223985890652557319223985890652557e-7L, 2.5052108385441718775052108385441718775052e-8L,
    2.0876756987868098979210090321201432312543e-9L, 1.6059043836821614599392377170154947932726e-10L,
    1.1470745597729724713851697978682105666233e-11L, 7.6471637318198164759011319857880704441551e-13L,
    4.7794773323873852974382074911175440275969e-14L, 2.8114572543455207631989455830103200162335e-15L,
    1.5619206968586226462216364350057333423519e-16L, 8.2206352466243297169559812368722807492207e-18L,
    4.1103176233121648584779906184361403746104e-19L, 1.9572941063391261230847574373505430355287e-20L,
    8.8967913924505732867488974425024683433125e-22L, 3.8681701706306840377169119315228123231793e-23L,
    1.6117375710961183490487133048011718013247e-24L, 6.4469502843844733961948532192046872052989e-26L,
    2.4795962632247974600749435458479566174227e-27L, 9.1836898637955461484257168364739133978617e-29L,
    3.2798892370698379101520417273121119278077e-30L, 1.1309962886447716931558764576938316992441e-31L,
    3.7699876288159056438529215256461056641468e-33L,
};

/* 1/(2k + 1) for k = 0 to 36. */
static const long double INV_ODD[] = {
    1.0000000000000000000000000000000000000000e+0L, 3.3333333333333333333333333333333333333333e-1L,
    2.0000000000000000000000000000000000000000e-1L, 1.4285714285714285714285714285714285714286e-1L,
    1.1111111111111111111111111111111111111111e-1L, 9.0909090909090909090909090909090909090909e-2L,
    7.6923076923076923076923076923076923076923e-2L, 6.6666666666666666666666666666666666666667e-2L,
    5.8823529411764705882352941176470588235294e-2L, 5.2631578947368421052631578947368421052632e-2L,
    4.7619047619047619047619047619047619047619e-2L, 4.3478260869565217391304347826086956521739e-2L,
    4.0000000000000000000000000000000000000000e-2L, 3.7037037037037037037037037037037037037037e-2L,
    3.4482758620689655172413793103448275862069e-2L, 3.2258064516129032258064516129032258064516e-2L,
    3.0303030303030303030303030303030303030303e-2L, 2.8571428571428571428571428571428571428571e-2L,
    2.7027027027027027027027027027027027027027e-2L, 2.5641025641025641025641025641025641025641e-2L,
    2.4390243902439024390243902439024390243902e-2L, 2.3255813953488372093023255813953488372093e-2L,
    2.2222222222222222222222222222222222222222e-2L, 2.1276595744680851063829787234042553191489e-2L,
    2.0408163265306122448979591836734693877551e-2L, 1.9607843137254901960784313725490196078431e-2L,
    1.8867924528301886792452830188679245283019e-2L, 1.8181818181818181818181818181818181818182e-2L,
    1.7543859649122807017543859649122807017544e-2L, 1.6949152542372881355932203389830508474576e-2L,
    1.6393442622950819672131147540983606557377e-2L, 1.5873015873015873015873015873015873015873e-2L,
    1.5384615384615384615384615384615384615385e-2L, 1.4925373134328358208955223880597014925373e-2L,
    1.4492753623188405797101449275362318840580e-2L, 1.4084507042253521126760563380281690140845e-2L,
    1.3698630136986301369863013698630136986301e-2L,
};

/* e**t - 1 for |t| <= ln 2 by the Taylor series. The remainder after the
 * last term is below 2**-113 of the result: |t|**30 / 31! < 1e-34 |t|. */
static long double expm1_series(long double t)
{
    int n = sizeof INV_FACTORIAL / sizeof INV_FACTORIAL[0] - 1;
    long double p = INV_FACTORIAL[n];
    for (int k = n - 1; k >= 1; k--)
        p = p * t + INV_FACTORIAL[k];
    return p * t;
}

/* sum (-1)**k t**(2k+1) / (2k+1) when alternate, else sum t**(2k+1) /
 * (2k+1), to n terms: atan(t) and atanh(t). */
static long double odd_series(long double t, int n, int alternate)
{
    long double t2 = t * t;
    long double p = INV_ODD[n - 1];
    for (int k = n - 2; k >= 0; k--)
        p = alternate ? INV_ODD[k] - t2 * p : INV_ODD[k] + t2 * p;
    return p * t;
}

/* The natural logarithm of a positive finite x as e * ln 2 + ln m with
 * m in [sqrt(1/2), sqrt(2)): ln m = 2 atanh(s), s = (m - 1) / (m + 1),
 * |s| <= 0.172, for which 24 terms reach binary128 precision. */
static long double log_positive(long double x, int *exponent, long double *mantissa_log)
{
    int e;
    long double m = frexpl(x, &e);
    if (m < 0.70710678118654752440084436210484903928L) {
        m *= 2.0L;
        e--;
    }
    long double s = (m - 1.0L) / (m + 1.0L);
    *exponent = e;
    *mantissa_log = 2.0L * odd_series(s, 24, 0);
    return e * LN2_HI + (e * LN2_LO + *mantissa_log);
}

/* atan(t) for 0 <= t <= 1: above tan(pi/12) the identity
 * atan(t) = pi/6 + atan((t sqrt(3) - 1) / (sqrt(3) + t)) brings the
 * argument within tan(pi/12), for which 30 terms are enough. */
static long double atan_unit(long double t)
{
    long double base = 0.0L;
    if (t > TAN_PI_12) {
        t = (t * SQRT3 - 1.0L) / (SQRT3 + t);
        base = PI_6;
    }
    return base + odd_series(t, 30, 1);
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
    long double a = fabsl(x);
    long double r = a > 1.0L ? PI_2 - atan_unit(1.0L / a) : atan_unit(a);
    return x < 0.0L ? -r : r;
}

long double atan2l(long double y, long double x)
{
    if (isnan(x))
        return x;
    if (isnan(y))
        return y;
    if (y == 0.0L) {
        if (signbit(x))
            return signbit(y) ? -M_PIL : M_PIL;
        return y;
    }
    if (x == 0.0L)
        return y > 0.0L ? PI_2 : -PI_2;
    if (isinf(x)) {
        if (isinf(y))
            return x > 0.0L ? copysignl(M_PI_4L, y) : copysignl(3.0L * M_PI_4L, y);
        return x > 0.0L ? copysignl(0.0L, y) : copysignl(M_PIL, y);
    }
    if (isinf(y))
        return copysignl(PI_2, y);
    long double r = atanl(fabsl(y / x));
    if (x < 0.0L)
        r = M_PIL - r;
    return y < 0.0L ? -r : r;
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

/* ---- exponentials and logarithms ---- */

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
    long double n = floorl(x + 0.5L);
    long double f = x - n;                          /* exact, |f| <= 0.5 */
    return ldexpl(1.0L + expm1_series(f * M_LN2L), (int)n);
}

long double expl(long double x)
{
    if (isnan(x) || x == INFINITY)
        return x;
    if (x == -INFINITY)
        return 0.0L;
    if (x > 11356.6L) {
        errno = ERANGE;
        return HUGE_VALL;
    }
    if (x < -11433.5L) {
        errno = ERANGE;
        return 0.0L;
    }
    /* x = n ln 2 + r with |r| <= ln 2 / 2, n ln 2 subtracted in two parts. */
    long double n = floorl(x * M_LOG2EL + 0.5L);
    long double r = (x - n * LN2_HI) - n * LN2_LO;
    return ldexpl(1.0L + expm1_series(r), (int)n);
}

long double expm1l(long double x)
{
    if (x == 0.0L || isnan(x))
        return x;
    if (fabsl(x) <= M_LN2L)
        return expm1_series(x);
    if (x < -80.0L)
        return -1.0L;                               /* e**x below 2**-115 */
    return expl(x) - 1.0L;
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
    int e;
    long double ln_m;
    return log_positive(x, &e, &ln_m);
}

long double log2l(long double x)
{
    if (x <= 0.0L || !isfinite(x))
        return logl(x) * M_LOG2EL;
    int e;
    long double ln_m;
    log_positive(x, &e, &ln_m);
    return (long double)e + ln_m * M_LOG2EL;        /* exact for powers of two */
}

long double log10l(long double x)
{
    return logl(x) * M_LOG10EL;
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
        return 2.0L * odd_series(x / (2.0L + x), 37, 0);
    }
    /* u = 1 + x rounded; (x - (u - 1)) / u corrects for the rounding. */
    long double u = 1.0L + x;
    return logl(u) + (x - (u - 1.0L)) / u;
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
