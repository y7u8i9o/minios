#include <math.h>
#include <float.h>
#include <errno.h>

static double narrow_double(long double value)
{
    double narrowed = (double)value;
    if (isfinite(value) && !isfinite(narrowed))
        errno = ERANGE;
    return narrowed;
}

static float narrow_float(long double value)
{
    float narrowed = (float)value;
    if (isfinite(value) && !isfinite(narrowed))
        errno = ERANGE;
    return narrowed;
}

long double atanl(long double x)
{
    if (isnan(x))
        return x;
    long double result;
    __asm__ volatile("fldt %1; fld1; fpatan; fstpt %0"
                     : "=m" (result) : "m" (x) : "st", "st(1)");
    return result;
}

long double atan2l(long double y, long double x)
{
    if (isnan(x))
        return x;
    if (isnan(y))
        return y;
    long double result;
    __asm__ volatile("fldt %1; fldt %2; fpatan; fstpt %0"
                     : "=m" (result) : "m" (y), "m" (x) : "st", "st(1)");
    return result;
}

long double asinl(long double x)
{
    if (isnan(x))
        return x;
    if (fabsl(x) > 1.0L) {
        errno = EDOM;
        return NAN;
    }
    if (x == 1.0L)
        return M_PI_2L;
    if (x == -1.0L)
        return -M_PI_2L;
    if (x == 0.0L)
        return x;
    return atan2l(x, sqrtl((1.0L - x) * (1.0L + x)));
}

long double acosl(long double x)
{
    if (isnan(x))
        return x;
    if (fabsl(x) > 1.0L) {
        errno = EDOM;
        return NAN;
    }
    if (x == 1.0L)
        return 0.0L;
    if (x == -1.0L)
        return M_PIL;
    return atan2l(sqrtl((1.0L - x) * (1.0L + x)), x);
}

double atan(double x) { return narrow_double(atanl((long double)x)); }
float atanf(float x) { return narrow_float(atanl((long double)x)); }
double atan2(double y, double x) { return narrow_double(atan2l((long double)y, (long double)x)); }
float atan2f(float y, float x) { return narrow_float(atan2l((long double)y, (long double)x)); }
double asin(double x) { return narrow_double(asinl((long double)x)); }
float asinf(float x) { return narrow_float(asinl((long double)x)); }
double acos(double x) { return narrow_double(acosl((long double)x)); }
float acosf(float x) { return narrow_float(acosl((long double)x)); }

long double sinhl(long double x)
{
    if (x == 0.0L || !isfinite(x))
        return x;
    long double magnitude = fabsl(x);
    long double result;
    if (magnitude <= 1.0L) {
        long double t = expm1l(magnitude);
        result = 0.5L * (t + t / (t + 1.0L));
    } else {
        long double half = expl(magnitude - M_LN2L);
        result = half - 0.25L / half;
    }
    return copysignl(result, x);
}

long double coshl(long double x)
{
    if (isnan(x))
        return x;
    if (isinf(x))
        return HUGE_VALL;
    long double magnitude = fabsl(x);
    if (magnitude <= 1.0L) {
        long double t = expm1l(magnitude);
        return 1.0L + t * t / (2.0L * (t + 1.0L));
    }
    long double half = expl(magnitude - M_LN2L);
    return half + 0.25L / half;
}

long double tanhl(long double x)
{
    if (x == 0.0L || isnan(x))
        return x;
    if (isinf(x))
        return copysignl(1.0L, x);
    long double magnitude = fabsl(x);
    if (magnitude > 32.0L)
        return copysignl(1.0L, x);
    long double t = expm1l(2.0L * magnitude);
    return copysignl(t / (t + 2.0L), x);
}

long double asinhl(long double x)
{
    if (x == 0.0L || !isfinite(x))
        return x;
    long double magnitude = fabsl(x);
    long double result;
    if (magnitude > 0x1p8191L) {
        result = logl(magnitude) + M_LN2L;
    } else {
        long double square = magnitude * magnitude;
        result = log1pl(magnitude + square / (1.0L + sqrtl(1.0L + square)));
    }
    return copysignl(result, x);
}

long double acoshl(long double x)
{
    if (isnan(x) || x == INFINITY)
        return x;
    if (x < 1.0L) {
        errno = EDOM;
        return NAN;
    }
    if (x == 1.0L)
        return 0.0L;
    if (x > 0x1p8191L)
        return logl(x) + M_LN2L;
    return log1pl((x - 1.0L) + sqrtl((x - 1.0L) * (x + 1.0L)));
}

long double atanhl(long double x)
{
    if (isnan(x) || x == 0.0L)
        return x;
    long double magnitude = fabsl(x);
    if (magnitude == 1.0L) {
        errno = ERANGE;
        return copysignl(HUGE_VALL, x);
    }
    if (magnitude > 1.0L) {
        errno = EDOM;
        return NAN;
    }
    long double result = 0.5L * (log1pl(magnitude) - log1pl(-magnitude));
    return copysignl(result, x);
}

double sinh(double x) { return narrow_double(sinhl((long double)x)); }
float sinhf(float x) { return narrow_float(sinhl((long double)x)); }
double cosh(double x) { return narrow_double(coshl((long double)x)); }
float coshf(float x) { return narrow_float(coshl((long double)x)); }
double tanh(double x) { return narrow_double(tanhl((long double)x)); }
float tanhf(float x) { return narrow_float(tanhl((long double)x)); }
double asinh(double x) { return narrow_double(asinhl((long double)x)); }
float asinhf(float x) { return narrow_float(asinhl((long double)x)); }
double acosh(double x) { return narrow_double(acoshl((long double)x)); }
float acoshf(float x) { return narrow_float(acoshl((long double)x)); }
double atanh(double x) { return narrow_double(atanhl((long double)x)); }
float atanhf(float x) { return narrow_float(atanhl((long double)x)); }
