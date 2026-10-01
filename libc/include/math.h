#pragma once
/* C floating point classification and the MiniOS scalar libm interface. The
 * implementation keeps floating point exceptions masked under the default
 * environment installed by the kernel; on x86_64 it targets the SSE2
 * baseline and the default MXCSR. */

#define FP_NAN       0
#define FP_INFINITE  1
#define FP_ZERO      2
#define FP_SUBNORMAL 3
#define FP_NORMAL    4

#define HUGE_VAL  (__builtin_huge_val())
#define HUGE_VALF (__builtin_huge_valf())
#define HUGE_VALL (__builtin_huge_vall())
#define INFINITY  (__builtin_inff())
#define NAN       (__builtin_nanf(""))

#define M_E        2.71828182845904523536
#define M_LOG2E    1.44269504088896340736
#define M_LOG10E   0.43429448190325182765
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_1_PI     0.31830988618379067154
#define M_2_PI     0.63661977236758134308
#define M_2_SQRTPI 1.12837916709551257390
#define M_SQRT2    1.41421356237309504880
#define M_SQRT1_2  0.70710678118654752440

#define M_EL        2.718281828459045235360287471352662498L
#define M_LOG2EL    1.442695040888963407359924681001892137L
#define M_LOG10EL   0.434294481903251827651128918916605082L
#define M_LN2L      0.693147180559945309417232121458176568L
#define M_LN10L     2.302585092994045684017991454684364208L
#define M_PIL       3.141592653589793238462643383279502884L
#define M_PI_2L     1.570796326794896619231321691639751442L
#define M_PI_4L     0.785398163397448309615660845819875721L
#define M_1_PIL     0.318309886183790671537767526745028724L
#define M_2_PIL     0.636619772367581343075535053490057448L
#define M_2_SQRTPIL 1.128379167095512573896158903121545172L
#define M_SQRT2L    1.414213562373095048801688724209698079L
#define M_SQRT1_2L  0.707106781186547524400844362104849039L

int __math_fpclassify(double x);
int __math_fpclassifyf(float x);
int __math_fpclassifyl(long double x);
int __math_signbit(double x);
int __math_signbitf(float x);
int __math_signbitl(long double x);

#define __math_pick(x, ff, fd, fl) _Generic((x), float: ff, long double: fl, default: fd)(x)
#define fpclassify(x) __math_pick((x), __math_fpclassifyf, __math_fpclassify, __math_fpclassifyl)
#define signbit(x)    __math_pick((x), __math_signbitf, __math_signbit, __math_signbitl)
#define isfinite(x)   (fpclassify(x) >= FP_ZERO)
#define isinf(x)      (fpclassify(x) == FP_INFINITE)
#define isnan(x)      (fpclassify(x) == FP_NAN)
#define isnormal(x)   (fpclassify(x) == FP_NORMAL)
#define isunordered(x, y) __builtin_isunordered((x), (y))
#define isgreater(x, y) __builtin_isgreater((x), (y))
#define isgreaterequal(x, y) __builtin_isgreaterequal((x), (y))
#define isless(x, y) __builtin_isless((x), (y))
#define islessequal(x, y) __builtin_islessequal((x), (y))
#define islessgreater(x, y) __builtin_islessgreater((x), (y))

double fabs(double x);
float fabsf(float x);
long double fabsl(long double x);
double copysign(double x, double y);
float copysignf(float x, float y);
long double copysignl(long double x, long double y);
double sqrt(double x);
float sqrtf(float x);
long double sqrtl(long double x);
double trunc(double x);
float truncf(float x);
long double truncl(long double x);
double floor(double x);
float floorf(float x);
long double floorl(long double x);
double ceil(double x);
float ceilf(float x);
long double ceill(long double x);
double round(double x);
float roundf(float x);
long double roundl(long double x);
double fmin(double x, double y);
float fminf(float x, float y);
long double fminl(long double x, long double y);
double fmax(double x, double y);
float fmaxf(float x, float y);
long double fmaxl(long double x, long double y);
double frexp(double x, int *exp);
float frexpf(float x, int *exp);
long double frexpl(long double x, int *exp);
double ldexp(double x, int exp);
float ldexpf(float x, int exp);
long double ldexpl(long double x, int exp);
double scalbn(double x, int exp);
float scalbnf(float x, int exp);
long double scalbnl(long double x, int exp);
double modf(double x, double *integer);
float modff(float x, float *integer);
long double modfl(long double x, long double *integer);
double fmod(double x, double y);
float fmodf(float x, float y);
long double fmodl(long double x, long double y);
double remainder(double x, double y);
float remainderf(float x, float y);
long double remainderl(long double x, long double y);
double exp(double x);
float expf(float x);
long double expl(long double x);
double exp2(double x);
float exp2f(float x);
long double exp2l(long double x);
double expm1(double x);
float expm1f(float x);
long double expm1l(long double x);
double log(double x);
float logf(float x);
long double logl(long double x);
double log2(double x);
float log2f(float x);
long double log2l(long double x);
double log10(double x);
float log10f(float x);
long double log10l(long double x);
double log1p(double x);
float log1pf(float x);
long double log1pl(long double x);
double pow(double x, double y);
float powf(float x, float y);
long double powl(long double x, long double y);
double hypot(double x, double y);
float hypotf(float x, float y);
long double hypotl(long double x, long double y);
double cbrt(double x);
float cbrtf(float x);
long double cbrtl(long double x);
double sin(double x);
float sinf(float x);
long double sinl(long double x);
double cos(double x);
float cosf(float x);
long double cosl(long double x);
double tan(double x);
float tanf(float x);
long double tanl(long double x);
double asin(double x);
float asinf(float x);
long double asinl(long double x);
double acos(double x);
float acosf(float x);
long double acosl(long double x);
double atan(double x);
float atanf(float x);
long double atanl(long double x);
double atan2(double y, double x);
float atan2f(float y, float x);
long double atan2l(long double y, long double x);
double sinh(double x);
float sinhf(float x);
long double sinhl(long double x);
double cosh(double x);
float coshf(float x);
long double coshl(long double x);
double tanh(double x);
float tanhf(float x);
long double tanhl(long double x);
double asinh(double x);
float asinhf(float x);
long double asinhl(long double x);
double acosh(double x);
float acoshf(float x);
long double acoshl(long double x);
double atanh(double x);
float atanhf(float x);
long double atanhl(long double x);
double nan(const char *tag);
float nanf(const char *tag);
long double nanl(const char *tag);
