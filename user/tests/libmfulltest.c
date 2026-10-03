/* Complete libm, floating environment and hexadecimal formatting test. */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <fenv.h>
#include <stdint.h>

static int failures;

#define CHECK(condition, ...) do { \
    if (!(condition)) { \
        failures++; \
        printf("libmfulltest: FAIL " __VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

static int near(double actual, double expected, double tolerance)
{
    return fabs(actual - expected) <= tolerance;
}

static int nearl(long double actual, long double expected, long double tolerance)
{
    return fabsl(actual - expected) <= tolerance;
}

static void test_inverse_and_hyperbolic(void)
{
    CHECK(near(asin(0.5), M_PI / 6.0, 2e-15), "asin");
    CHECK(near(acos(0.5), M_PI / 3.0, 2e-15), "acos");
    CHECK(near(atan(1.0), M_PI_4, 2e-15), "atan");
    CHECK(near(atan2(1.0, -1.0), 3.0 * M_PI_4, 2e-15), "atan2 quadrant");
    CHECK(signbit(atan2(-0.0, 1.0)), "atan2 signed zero");
    errno = 0;
    CHECK(isnan(asin(1.01)) && errno == EDOM, "asin domain");

    CHECK(near(sinh(M_LN2), 0.75, 2e-15), "sinh");
    CHECK(near(cosh(M_LN2), 1.25, 2e-15), "cosh");
    CHECK(near(tanh(M_LN2), 0.6, 2e-15), "tanh");
    CHECK(near(asinh(0.75), M_LN2, 2e-15), "asinh");
    CHECK(near(acosh(1.25), M_LN2, 2e-15), "acosh");
    CHECK(near(atanh(0.6), M_LN2, 2e-15), "atanh");
    CHECK(signbit(tanh(-0.0)) && signbit(asinh(-0.0)) && signbit(atanh(-0.0)),
          "hyperbolic signed zero");
    errno = 0;
    CHECK(isnan(acosh(0.5)) && errno == EDOM, "acosh domain");
    errno = 0;
    CHECK(isinf(atanh(1.0)) && errno == ERANGE, "atanh pole");
}

static void test_long_double_family(void)
{
    const long double fine = 0x1.0000000000000002p+0L;
    CHECK(fabsl(-fine) == fine && signbit(copysignl(fine, -1.0L)),
          "long absolute and sign");
    CHECK(nearl(sqrtl(2.0L) * sqrtl(2.0L), 2.0L, 2e-18L), "sqrtl");
    CHECK(floorl(-1.25L) == -2.0L && ceill(-1.25L) == -1.0L &&
          truncl(-1.25L) == -1.0L && roundl(1.5L) == 2.0L, "long rounding");
    CHECK(signbit(fminl(0.0L, -0.0L)) && !signbit(fmaxl(0.0L, -0.0L)),
          "long min max");

    int exponent;
    long double fraction = frexpl(10.0L, &exponent);
    CHECK(fraction == 0.625L && exponent == 4 && ldexpl(fraction, exponent) == 10.0L,
          "long frexp ldexp");
    CHECK(scalbnl(1.0L, -16445) == LDBL_TRUE_MIN, "long true minimum scaling");
    long double integer;
    CHECK(modfl(-3.25L, &integer) == -0.25L && integer == -3.0L, "modfl");
    CHECK(fmodl(5.5L, 2.0L) == 1.5L && remainderl(5.5L, 2.0L) == -0.5L,
          "long remainders");

    CHECK(nearl(expl(logl(fine)), fine, 2e-18L), "long exp log precision");
    CHECK(exp2l(10.0L) == 1024.0L && log2l(0x1p1000L) == 1000.0L,
          "long exp2 log2");
    CHECK(nearl(expm1l(1e-12L), 1.0000000000005e-12L, 2e-30L), "expm1l");
    CHECK(nearl(log1pl(1e-12L), 9.999999999995e-13L, 2e-30L), "log1pl");
    CHECK(nearl(log10l(1000.0L), 3.0L, 2e-18L), "log10l");
    CHECK(nearl(powl(2.0L, 0.5L), sqrtl(2.0L), 2e-18L), "powl");
    CHECK(hypotl(3.0L, 4.0L) == 5.0L && nearl(cbrtl(27.0L), 3.0L, 2e-18L),
          "hypotl cbrtl");

    CHECK(nearl(asinl(0.5L), M_PIL / 6.0L, 2e-18L) &&
          nearl(acosl(0.5L), M_PIL / 3.0L, 2e-18L) &&
          nearl(atanl(1.0L), M_PI_4L, 2e-18L), "long inverse trig");
    CHECK(nearl(sinhl(M_LN2L), 0.75L, 2e-18L) &&
          nearl(coshl(M_LN2L), 1.25L, 2e-18L) &&
          nearl(tanhl(M_LN2L), 0.6L, 2e-18L), "long hyperbolic");
    CHECK(nearl(asinhl(0.75L), M_LN2L, 2e-18L) &&
          nearl(acoshl(1.25L), M_LN2L, 2e-18L) &&
          nearl(atanhl(0.6L), M_LN2L, 2e-18L), "long inverse hyperbolic");
    CHECK(isnan(nanl("payload")), "nanl");
}

static void test_large_trigonometry(void)
{
    CHECK(near(sin(1e20), -0.64525128526578079, 3e-16), "sin 1e20");
    CHECK(near(cos(1e100), 0.92472423875193377, 3e-16), "cos 1e100");
    CHECK(near(tan(1e300), 1.4214488238747243, 8e-16), "tan 1e300");
    double huge_sine = sin(1e300), huge_cosine = cos(1e300);
    CHECK(near(huge_sine * huge_sine + huge_cosine * huge_cosine, 1.0, 4e-16),
          "huge trig identity");

    long double enormous = 0x1p16000L;
    long double enormous_sine = sinl(enormous);
    long double enormous_cosine = cosl(enormous);
    CHECK(nearl(enormous_sine, 0.699245882207296475024404079288278640L, 4e-18L),
          "sinl 2^16000 got %La", enormous_sine);
    CHECK(nearl(enormous_cosine, 0.714881246233344453181191532977609171L, 4e-18L),
          "cosl 2^16000 got %La", enormous_cosine);
}

static void test_environment(void)
{
    CHECK(fesetenv(FE_DFL_ENV) == 0 && fegetround() == FE_TONEAREST,
          "default environment");
    volatile double one = 1.0;
    volatile double half_ulp = 0x1p-53;
    volatile double rounded;
    union { double value; uint64_t bits; } bits;

    CHECK(fesetround(FE_UPWARD) == 0 && fegetround() == FE_UPWARD,
          "set upward rounding");
    CHECK(FLT_ROUNDS == 2, "FLT_ROUNDS upward");
    rounded = one + half_ulp;
    bits.value = rounded;
    CHECK(bits.bits == 0x3ff0000000000001ULL, "upward arithmetic rounding");
    CHECK(fesetround(FE_TOWARDZERO) == 0, "set toward-zero rounding");
    CHECK(FLT_ROUNDS == 0, "FLT_ROUNDS toward zero");
    rounded = one + half_ulp;
    CHECK(rounded == 1.0, "toward-zero arithmetic rounding");
    CHECK(fesetround(0x1234) == -1, "reject invalid rounding mode");
    fesetround(FE_TONEAREST);

    feclearexcept(FE_ALL_EXCEPT);
    volatile double zero = 0.0;
    rounded = one / zero;
    (void)rounded;
    CHECK(fetestexcept(FE_DIVBYZERO) == FE_DIVBYZERO, "hardware divide-by-zero flag");
    feclearexcept(FE_ALL_EXCEPT);
    feraiseexcept(FE_INVALID | FE_INEXACT);
    CHECK(fetestexcept(FE_ALL_EXCEPT) == (FE_INVALID | FE_INEXACT),
          "raise exception flags");
    fexcept_t saved;
    fegetexceptflag(&saved, FE_ALL_EXCEPT);
    feclearexcept(FE_ALL_EXCEPT);
    fesetexceptflag(&saved, FE_INVALID);
    CHECK(fetestexcept(FE_ALL_EXCEPT) == FE_INVALID, "save and restore exception flag");

    fenv_t environment;
    feraiseexcept(FE_OVERFLOW);
    feholdexcept(&environment);
    CHECK(fetestexcept(FE_ALL_EXCEPT) == 0, "feholdexcept clears flags");
    feraiseexcept(FE_UNDERFLOW);
    feupdateenv(&environment);
    CHECK((fetestexcept(FE_ALL_EXCEPT) & (FE_INVALID | FE_OVERFLOW | FE_UNDERFLOW)) ==
          (FE_INVALID | FE_OVERFLOW | FE_UNDERFLOW), "update merges flags");
    fesetenv(FE_DFL_ENV);
}

static void test_hexadecimal_format(void)
{
    char buffer[128];
    snprintf(buffer, sizeof(buffer), "%a", 1.5);
    CHECK(strcmp(buffer, "0x1.8p+0") == 0, "%%a got %s", buffer);
    snprintf(buffer, sizeof(buffer), "%.3a", 1.5);
    CHECK(strcmp(buffer, "0x1.800p+0") == 0, "%%.3a got %s", buffer);
    snprintf(buffer, sizeof(buffer), "%A", 1.5);
    CHECK(strcmp(buffer, "0X1.8P+0") == 0, "%%A got %s", buffer);
    snprintf(buffer, sizeof(buffer), "%a", DBL_TRUE_MIN);
    CHECK(strcmp(buffer, "0x1p-1074") == 0, "subnormal %%a got %s", buffer);
    snprintf(buffer, sizeof(buffer), "%#.0a", 1.0);
    CHECK(strcmp(buffer, "0x1.p+0") == 0, "alternate %%a got %s", buffer);
    snprintf(buffer, sizeof(buffer), "%a", -0.0);
    CHECK(strcmp(buffer, "-0x0p+0") == 0, "negative zero %%a got %s", buffer);
    snprintf(buffer, sizeof(buffer), "%La", 0x1.0000000000000002p+0L);
    CHECK(strcmp(buffer, "0x1.0000000000000002p+0") == 0, "%%La got %s", buffer);

    fesetround(FE_TONEAREST);
    snprintf(buffer, sizeof(buffer), "%.0a", 1.25);
    CHECK(strcmp(buffer, "0x1p+0") == 0, "nearest hex rounding got %s", buffer);
    fesetround(FE_UPWARD);
    snprintf(buffer, sizeof(buffer), "%.0a", 1.25);
    CHECK(strcmp(buffer, "0x1p+1") == 0, "upward hex rounding got %s", buffer);
    fesetenv(FE_DFL_ENV);
}

int main(void)
{
    test_inverse_and_hyperbolic();
    test_long_double_family();
    test_large_trigonometry();
    test_environment();
    test_hexadecimal_format();
    printf("libmfulltest: %d failures\n", failures);
    return failures ? 1 : 0;
}
