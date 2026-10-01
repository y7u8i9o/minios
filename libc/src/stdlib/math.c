#include <math.h>
#include <errno.h>
#include <stdint.h>
#include "math_arch.h"
#include "../ldouble.h"

union double_bits { double value; uint64_t bits; };
union float_bits { float value; uint32_t bits; };

int __math_fpclassify(double x)
{
    union double_bits u = { x };
    uint64_t exp = u.bits >> 52 & 0x7ff, frac = u.bits & ((1ULL << 52) - 1);
    if (exp == 0x7ff)
        return frac ? FP_NAN : FP_INFINITE;
    if (!exp)
        return frac ? FP_SUBNORMAL : FP_ZERO;
    return FP_NORMAL;
}

int __math_fpclassifyf(float x)
{
    union float_bits u = { x };
    uint32_t exp = u.bits >> 23 & 0xff, frac = u.bits & ((1U << 23) - 1);
    if (exp == 0xff)
        return frac ? FP_NAN : FP_INFINITE;
    if (!exp)
        return frac ? FP_SUBNORMAL : FP_ZERO;
    return FP_NORMAL;
}

int __math_fpclassifyl(long double x)
{
    struct ld_parts p = ld_split(x);
    if (p.raw_exponent == 0x7fff)
        return p.significand == 0x8000000000000000ULL && !p.lower ? FP_INFINITE : FP_NAN;
    if (!p.raw_exponent)
        return p.significand || p.lower ? FP_SUBNORMAL : FP_ZERO;
    return FP_NORMAL;
}

int __math_signbit(double x)
{
    union double_bits u = { x };
    return (int)(u.bits >> 63);
}

int __math_signbitf(float x)
{
    union float_bits u = { x };
    return (int)(u.bits >> 31);
}

int __math_signbitl(long double x)
{
    return (int)ld_split(x).negative;
}

double fabs(double x)
{
    union double_bits u = { x };
    u.bits &= ~(1ULL << 63);
    return u.value;
}

float fabsf(float x)
{
    union float_bits u = { x };
    u.bits &= ~(1U << 31);
    return u.value;
}

double copysign(double x, double y)
{
    union double_bits ux = { x }, uy = { y };
    ux.bits = (ux.bits & ~(1ULL << 63)) | (uy.bits & (1ULL << 63));
    return ux.value;
}

float copysignf(float x, float y)
{
    union float_bits ux = { x }, uy = { y };
    ux.bits = (ux.bits & ~(1U << 31)) | (uy.bits & (1U << 31));
    return ux.value;
}

double trunc(double x)
{
    union double_bits u = { x };
    int exp = (int)(u.bits >> 52 & 0x7ff) - 1023;
    if (exp < 0) {
        u.bits &= 1ULL << 63;
        return u.value;
    }
    if (exp >= 52)
        return x;
    u.bits &= ~((1ULL << (52 - exp)) - 1);
    return u.value;
}

float truncf(float x)
{
    union float_bits u = { x };
    int exp = (int)(u.bits >> 23 & 0xff) - 127;
    if (exp < 0) {
        u.bits &= 1U << 31;
        return u.value;
    }
    if (exp >= 23)
        return x;
    u.bits &= ~((1U << (23 - exp)) - 1);
    return u.value;
}

double floor(double x)
{
    double whole = trunc(x);
    return x < whole ? whole - 1.0 : whole;
}

float floorf(float x)
{
    float whole = truncf(x);
    return x < whole ? whole - 1.0f : whole;
}

double ceil(double x)
{
    double whole = trunc(x);
    return x > whole ? whole + 1.0 : whole;
}

float ceilf(float x)
{
    float whole = truncf(x);
    return x > whole ? whole + 1.0f : whole;
}

double round(double x)
{
    if (x == 0.0 || !isfinite(x))
        return x;
    return x < 0.0 ? ceil(x - 0.5) : floor(x + 0.5);
}

float roundf(float x)
{
    if (x == 0.0f || !isfinite(x))
        return x;
    return x < 0.0f ? ceilf(x - 0.5f) : floorf(x + 0.5f);
}

double fmin(double x, double y)
{
    if (isnan(x)) return y;
    if (isnan(y)) return x;
    if (x == y && x == 0.0)
        return signbit(x) ? x : y;
    return x < y ? x : y;
}

float fminf(float x, float y)
{
    if (isnan(x)) return y;
    if (isnan(y)) return x;
    if (x == y && x == 0.0f)
        return signbit(x) ? x : y;
    return x < y ? x : y;
}

double fmax(double x, double y)
{
    if (isnan(x)) return y;
    if (isnan(y)) return x;
    if (x == y && x == 0.0)
        return signbit(x) ? y : x;
    return x > y ? x : y;
}

float fmaxf(float x, float y)
{
    if (isnan(x)) return y;
    if (isnan(y)) return x;
    if (x == y && x == 0.0f)
        return signbit(x) ? y : x;
    return x > y ? x : y;
}

double frexp(double x, int *exp)
{
    union double_bits u = { x };
    uint64_t raw = u.bits >> 52 & 0x7ff;
    *exp = 0;
    if (!raw) {
        if ((u.bits & ((1ULL << 52) - 1)) == 0)
            return x;
        x *= 0x1p54;
        u.value = x;
        raw = u.bits >> 52 & 0x7ff;
        *exp = (int)raw - 1022 - 54;
    } else if (raw == 0x7ff) {
        return x;
    } else {
        *exp = (int)raw - 1022;
    }
    u.bits = (u.bits & ((1ULL << 63) | ((1ULL << 52) - 1))) | (1022ULL << 52);
    return u.value;
}

float frexpf(float x, int *exp)
{
    union float_bits u = { x };
    uint32_t raw = u.bits >> 23 & 0xff;
    *exp = 0;
    if (!raw) {
        if ((u.bits & ((1U << 23) - 1)) == 0)
            return x;
        x *= 0x1p25f;
        u.value = x;
        raw = u.bits >> 23 & 0xff;
        *exp = (int)raw - 126 - 25;
    } else if (raw == 0xff) {
        return x;
    } else {
        *exp = (int)raw - 126;
    }
    u.bits = (u.bits & ((1U << 31) | ((1U << 23) - 1))) | (126U << 23);
    return u.value;
}

static double power_two(int exp)
{
    union double_bits u = { .bits = (uint64_t)(exp + 1023) << 52 };
    return u.value;
}

double ldexp(double x, int exp)
{
    if (x == 0.0 || !isfinite(x))
        return x;
    double original = x;
    while (exp > 0) {
        int step = exp > 512 ? 512 : exp;
        x *= power_two(step);
        exp -= step;
    }
    while (exp < 0) {
        int step = exp < -512 ? -512 : exp;
        x *= power_two(step);
        exp -= step;
    }
    if (!isfinite(x) || (x == 0.0 && original != 0.0))
        errno = ERANGE;
    return x;
}

float ldexpf(float x, int exp)
{
    double result = ldexp((double)x, exp);
    float narrowed = (float)result;
    if ((!isfinite(narrowed) && isfinite(x)) || (narrowed == 0.0f && x != 0.0f))
        errno = ERANGE;
    return narrowed;
}

double scalbn(double x, int exp) { return ldexp(x, exp); }
float scalbnf(float x, int exp) { return ldexpf(x, exp); }

double modf(double x, double *integer)
{
    if (x == 0.0 || isinf(x)) {
        *integer = x;
        return copysign(0.0, x);
    }
    if (isnan(x)) {
        *integer = x;
        return x;
    }
    *integer = trunc(x);
    return x - *integer;
}

float modff(float x, float *integer)
{
    if (x == 0.0f || isinf(x)) {
        *integer = x;
        return copysignf(0.0f, x);
    }
    if (isnan(x)) {
        *integer = x;
        return x;
    }
    *integer = truncf(x);
    return x - *integer;
}

double fmod(double x, double y)
{
    if (isnan(x)) return x;
    if (isnan(y)) return y;
    if (isinf(x) || y == 0.0) {
        errno = EDOM;
        return NAN;
    }
    if (isinf(y) || x == 0.0)
        return x;
    return __math_partial_remainder(x, y, 0);
}

float fmodf(float x, float y)
{
    return (float)fmod((double)x, (double)y);
}

double remainder(double x, double y)
{
    if (isnan(x)) return x;
    if (isnan(y)) return y;
    if (isinf(x) || y == 0.0) {
        errno = EDOM;
        return NAN;
    }
    if (isinf(y) || x == 0.0)
        return x;
    return __math_partial_remainder(x, y, 1);
}

float remainderf(float x, float y)
{
    return (float)remainder((double)x, (double)y);
}

static const double ln2_high = 6.93147180369123816490e-01;
static const double ln2_low = 1.90821492927058770002e-10;

static double exp_reduced(double x)
{
    double term = 1.0, sum = 1.0;
    for (int divisor = 1; divisor <= 18; divisor++) {
        term *= x / (double)divisor;
        sum += term;
    }
    return sum;
}

double exp(double x)
{
    if (isnan(x) || x == INFINITY)
        return x;
    if (x == -INFINITY)
        return 0.0;
    if (x > 7.09782712893383973096e2) {
        errno = ERANGE;
        return HUGE_VAL;
    }
    if (x < -7.45133219101941108420e2) {
        errno = ERANGE;
        return 0.0;
    }
    int exponent = (int)round(x * M_LOG2E);
    double reduced = (x - exponent * ln2_high) - exponent * ln2_low;
    return ldexp(exp_reduced(reduced), exponent);
}

double exp2(double x)
{
    if (isnan(x) || x == INFINITY)
        return x;
    if (x == -INFINITY)
        return 0.0;
    if (x >= 1024.0) {
        errno = ERANGE;
        return HUGE_VAL;
    }
    if (x <= -1075.0) {
        errno = ERANGE;
        return 0.0;
    }
    int exponent = (int)round(x);
    return ldexp(exp_reduced((x - exponent) * M_LN2), exponent);
}

double expm1(double x)
{
    if (x == 0.0)
        return x;
    if (fabs(x) < 1e-5) {
        double term = x, sum = x;
        for (int divisor = 2; divisor <= 12; divisor++) {
            term *= x / (double)divisor;
            sum += term;
        }
        return sum;
    }
    return exp(x) - 1.0;
}

static float narrow_float(double value)
{
    float narrowed = (float)value;
    if (isfinite(value) && ((!isfinite(narrowed)) || (narrowed == 0.0f && value != 0.0)))
        errno = ERANGE;
    return narrowed;
}

float expf(float x) { return narrow_float(exp((double)x)); }
float exp2f(float x) { return narrow_float(exp2((double)x)); }
float expm1f(float x) { return narrow_float(expm1((double)x)); }

double log(double x)
{
    if (isnan(x) || x == INFINITY)
        return x;
    if (x == 0.0) {
        errno = ERANGE;
        return -HUGE_VAL;
    }
    if (x < 0.0) {
        errno = EDOM;
        return NAN;
    }
    int exponent;
    double mantissa = frexp(x, &exponent);
    if (mantissa < M_SQRT1_2) {
        mantissa *= 2.0;
        exponent--;
    }
    double z = (mantissa - 1.0) / (mantissa + 1.0);
    double z2 = z * z, term = z, sum = z;
    for (int divisor = 3; divisor <= 35; divisor += 2) {
        term *= z2;
        sum += term / (double)divisor;
    }
    return (2.0 * sum + exponent * ln2_high) + exponent * ln2_low;
}

double log2(double x)
{
    return log(x) * M_LOG2E;
}

double log10(double x)
{
    return log(x) * M_LOG10E;
}

double log1p(double x)
{
    if (x == 0.0)
        return x;
    if (isnan(x) || x == INFINITY)
        return x;
    if (x == -1.0) {
        errno = ERANGE;
        return -HUGE_VAL;
    }
    if (x < -1.0) {
        errno = EDOM;
        return NAN;
    }
    if (fabs(x) <= 0.125) {
        double term = x, sum = 0.0;
        for (int n = 1; n <= 40; n++) {
            sum += (n & 1 ? 1.0 : -1.0) * term / (double)n;
            term *= x;
        }
        return sum;
    }
    return log(1.0 + x);
}

float logf(float x) { return (float)log((double)x); }
float log2f(float x) { return (float)log2((double)x); }
float log10f(float x) { return (float)log10((double)x); }
float log1pf(float x) { return (float)log1p((double)x); }

static int integer_is_odd(double value)
{
    value = fabs(value);
    if (value >= 0x1p53)
        return 0;
    return ((uint64_t)value & 1U) != 0;
}

double pow(double x, double y)
{
    if (y == 0.0 || x == 1.0)
        return 1.0;
    if (isnan(x) || isnan(y))
        return NAN;
    double magnitude = fabs(x);
    if (isinf(y)) {
        if (magnitude == 1.0)
            return 1.0;
        if (y > 0.0)
            return magnitude > 1.0 ? HUGE_VAL : 0.0;
        return magnitude > 1.0 ? 0.0 : HUGE_VAL;
    }
    int odd = integer_is_odd(y);
    if (x == 0.0) {
        if (y < 0.0) {
            errno = ERANGE;
            return signbit(x) && odd ? -HUGE_VAL : HUGE_VAL;
        }
        return signbit(x) && odd ? -0.0 : 0.0;
    }
    if (x < 0.0 && trunc(y) != y) {
        errno = EDOM;
        return NAN;
    }
    /* On x86_64 the x87 kernels carry 64 significand bits, so the double
     * result is correctly rounded except in rare double rounding cases.
     * The double kernels lost up to one unit through exp(y * log(x)):
     * 2 ** 0.5 came out one unit below sqrt(2). The aarch64 long double
     * kernels compute in double precision (arch/aarch64/math_long.c). */
    double result = (double)exp2l((long double)y * log2l((long double)magnitude));
    if (isinf(result) || result == 0.0)
        errno = ERANGE;
    return x < 0.0 && odd ? -result : result;
}

float powf(float x, float y)
{
    return narrow_float(pow((double)x, (double)y));
}

double hypot(double x, double y)
{
    x = fabs(x);
    y = fabs(y);
    if (isinf(x) || isinf(y))
        return HUGE_VAL;
    if (isnan(x) || isnan(y))
        return NAN;
    if (x < y) {
        double temporary = x;
        x = y;
        y = temporary;
    }
    if (x == 0.0)
        return 0.0;
    double ratio = y / x;
    double result = x * sqrt(1.0 + ratio * ratio);
    if (isinf(result))
        errno = ERANGE;
    return result;
}

float hypotf(float x, float y)
{
    return narrow_float(hypot((double)x, (double)y));
}

double cbrt(double x)
{
    if (x == 0.0 || !isfinite(x))
        return x;
    double magnitude = fabs(x);
    double estimate = exp(log(magnitude) / 3.0);
    for (int i = 0; i < 5; i++)
        estimate = (2.0 * estimate + magnitude / (estimate * estimate)) / 3.0;
    return copysign(estimate, x);
}

float cbrtf(float x) { return (float)cbrt((double)x); }

double nan(const char *tag)
{
    (void)tag;
    return __builtin_nan("");
}

float nanf(const char *tag)
{
    (void)tag;
    return __builtin_nanf("");
}
