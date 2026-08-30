/* Floating point runtime and SSE2 vector API integration test. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <float.h>
#include <math.h>
#include <minios/simd.h>

static int failures;

#define CHECK(condition, ...) do { \
    if (!(condition)) { \
        failures++; \
        printf("floattest: FAIL " __VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

static int near(double a, double b, double tolerance)
{
    return fabs(a - b) <= tolerance;
}

static void test_math(void)
{
    double root = sqrt(2.0);
    CHECK(near(root * root, 2.0, 1e-12), "sqrt accuracy");
    errno = 0;
    CHECK(isnan(sqrt(-1.0)) && errno == EDOM, "sqrt negative domain");
    CHECK(floor(-1.25) == -2.0, "floor negative");
    CHECK(ceil(-1.25) == -1.0, "ceil negative");
    CHECK(trunc(-1.75) == -1.0, "trunc negative");
    CHECK(round(-1.5) == -2.0 && round(1.5) == 2.0, "round away from zero");
    CHECK(signbit(round(-0.0)), "round signed zero");

    int exponent = 0;
    double fraction = frexp(10.0, &exponent);
    CHECK(fraction == 0.625 && exponent == 4, "frexp decomposition");
    CHECK(ldexp(fraction, exponent) == 10.0, "ldexp reconstruction");
    CHECK(fpclassify(ldexp(1.0, -1074)) == FP_SUBNORMAL, "smallest subnormal");
    errno = 0;
    CHECK(isinf(ldexp(1.0, 1024)) && errno == ERANGE, "ldexp overflow");

    double integer = 0.0;
    CHECK(modf(-3.25, &integer) == -0.25 && integer == -3.0, "modf split");
    CHECK(signbit(modf(-INFINITY, &integer)) && isinf(integer) && signbit(integer),
          "modf infinity");
    CHECK(fmin(NAN, 4.0) == 4.0 && fmax(4.0, NAN) == 4.0, "min max NaN");
    CHECK(signbit(fmin(0.0, -0.0)), "fmin signed zero");
    CHECK(!signbit(fmax(0.0, -0.0)), "fmax signed zero");

    CHECK(near(fmod(5.5, 2.0), 1.5, 1e-15), "fmod remainder");
    CHECK(near(remainder(5.5, 2.0), -0.5, 1e-15), "IEEE remainder");
    errno = 0;
    CHECK(isnan(fmod(1.0, 0.0)) && errno == EDOM, "fmod zero divisor");

    CHECK(near(exp(1.0), M_E, 2e-15), "exp accuracy");
    CHECK(near(exp2(0.5), M_SQRT2, 2e-15), "exp2 accuracy");
    CHECK(near(expm1(1e-10), 1.00000000005e-10, 1e-24), "expm1 small argument");
    CHECK(signbit(expm1(-0.0)), "expm1 signed zero");
    CHECK(near(log(M_E), 1.0, 2e-15), "log accuracy");
    CHECK(near(log2(1024.0), 10.0, 2e-14), "log2 accuracy");
    CHECK(near(log10(1000.0), 3.0, 2e-14), "log10 accuracy");
    CHECK(near(log1p(1e-10), 9.9999999995e-11, 1e-24), "log1p small argument");
    CHECK(signbit(log1p(-0.0)), "log1p signed zero");
    errno = 0;
    CHECK(isnan(log(-1.0)) && errno == EDOM, "log negative domain");
    errno = 0;
    CHECK(isinf(log(0.0)) && signbit(log(0.0)) && errno == ERANGE, "log zero range");

    CHECK(near(pow(2.0, 10.0), 1024.0, 2e-11), "pow positive base");
    CHECK(near(pow(-2.0, 3.0), -8.0, 2e-14), "pow negative integer");
    errno = 0;
    CHECK(isnan(pow(-2.0, 0.5)) && errno == EDOM, "pow negative fractional domain");
    CHECK(hypot(3.0, 4.0) == 5.0, "hypot scale");
    CHECK(near(cbrt(27.0), 3.0, 2e-14), "cbrt accuracy");

    CHECK(near(sin(M_PI / 6.0), 0.5, 2e-15), "sin accuracy");
    CHECK(near(cos(M_PI), -1.0, 2e-15), "cos accuracy");
    CHECK(near(tan(M_PI / 4.0), 1.0, 2e-15), "tan accuracy");
    errno = 0;
    CHECK(isnan(sin(INFINITY)) && errno == EDOM, "sin infinity domain");

    CHECK(FLT_RADIX == 2 && FLT_MANT_DIG == 24 && DBL_MANT_DIG == 53,
          "float format constants");
    CHECK(fpclassify(FLT_TRUE_MIN) == FP_SUBNORMAL &&
          fpclassify(DBL_TRUE_MIN) == FP_SUBNORMAL, "true minimum constants");
    CHECK(fpclassify(LDBL_MAX) == FP_NORMAL && signbit(-0.0L),
          "long double classification");
}

static void test_conversion(void)
{
    char *end;
    double value = strtod("  -6.25e-3rest", &end);
    CHECK(near(value, -0.00625, 1e-15) && strcmp(end, "rest") == 0,
          "strtod decimal and end pointer");
    value = strtod("1e+tail", &end);
    CHECK(value == 1.0 && strcmp(end, "e+tail") == 0, "strtod incomplete exponent");
    CHECK(isinf(strtod("-INFINITY", &end)) && signbit(strtod("-INFINITY", NULL)),
          "strtod infinity");
    CHECK(isnan(strtod("nan(payload)", &end)) && *end == '\0', "strtod NaN payload");
    errno = 0;
    CHECK(isinf(strtod("1e309", NULL)) && errno == ERANGE, "strtod overflow");
    value = strtod("0x1.8p+2tail", &end);
    CHECK(value == 6.0 && strcmp(end, "tail") == 0, "strtod hexadecimal");
    CHECK(strtof("0x1p-149", &end) == FLT_TRUE_MIN && *end == '\0',
          "strtof smallest subnormal");
    errno = 0;
    CHECK(isinf(strtof("0x1p+128", NULL)) && errno == ERANGE, "strtof overflow");

    char buffer[64];
    snprintf(buffer, sizeof(buffer), "%+.2f", 12.345);
    CHECK(strcmp(buffer, "+12.35") == 0, "fixed format got %s", buffer);
    snprintf(buffer, sizeof(buffer), "%.3e", 12.345);
    CHECK(strcmp(buffer, "1.235e+01") == 0, "scientific format got %s", buffer);
    snprintf(buffer, sizeof(buffer), "%.4g", 12.345);
    CHECK(strcmp(buffer, "12.35") == 0, "general fixed format got %s", buffer);
    snprintf(buffer, sizeof(buffer), "%.6g", 1.25e-7);
    CHECK(strcmp(buffer, "1.25e-07") == 0, "general exponent format got %s", buffer);
    snprintf(buffer, sizeof(buffer), "%08.2f", -1.5);
    CHECK(strcmp(buffer, "-0001.50") == 0, "zero padded format got %s", buffer);
    snprintf(buffer, sizeof(buffer), "%F", INFINITY);
    CHECK(strcmp(buffer, "INF") == 0, "uppercase infinity got %s", buffer);
}

static void test_vectors(void)
{
    simd_f32x4 a = simd_set_f32x4(1.0f, 2.0f, 3.0f, 4.0f);
    simd_f32x4 b = simd_set_f32x4(5.0f, 6.0f, 7.0f, 8.0f);
    CHECK(simd_dot_f32x4(a, b) == 70.0f, "f32x4 expression");
    simd_f64x2 da = simd_set_f64x2(1.5, -2.0);
    simd_f64x2 db = simd_set_f64x2(2.0, 4.0);
    CHECK(simd_dot_f64x2(da, db) == -5.0, "f64x2 expression");
    simd_f32x4 roots = simd_sqrt_f32x4(simd_abs_f32x4(
        simd_set_f32x4(-4.0f, 9.0f, -16.0f, 25.0f)));
    CHECK(roots[0] == 2.0f && roots[1] == 3.0f && roots[2] == 4.0f && roots[3] == 5.0f,
          "packed absolute square root");
    simd_f32x4 clamped = simd_clamp_f32x4(
        simd_set_f32x4(-2.0f, 0.5f, 5.0f, 20.0f),
        simd_splat_f32x4(0.0f), simd_splat_f32x4(10.0f));
    CHECK(clamped[0] == 0.0f && clamped[1] == 0.5f &&
          clamped[2] == 5.0f && clamped[3] == 10.0f, "packed clamp");

    float xs[20] __attribute__((aligned(16)));
    float ys[20] __attribute__((aligned(16)));
    float output[20] __attribute__((aligned(16)));
    float *x = xs + 1, *y = ys + 1, *destination = output + 1;
    double expected_dot = 0.0;
    for (int i = 0; i < 19; i++) {
        x[i] = (float)(i + 1);
        y[i] = (float)(i * 2);
        expected_dot += (double)x[i] * y[i];
    }
    simd_saxpy_f32(destination, x, y, 2.0f, 19);
    for (int i = 0; i < 19; i++)
        CHECK(destination[i] == (float)(4 * i + 2), "SAXPY lane %d", i);
    CHECK(near(simd_dot_f32(x, y, 19), expected_dot, 1e-3), "f32 array dot");
    simd_mul_f32(destination, x, y, 19);
    CHECK(destination[0] == 0.0f && destination[18] == 684.0f, "f32 multiply with tail");
    simd_scale_f32(destination, x, 0.5f, 19);
    CHECK(destination[0] == 0.5f && destination[18] == 9.5f, "f32 scale with tail");
    simd_sqrt_f32(destination, x, 19);
    CHECK(destination[0] == 1.0f && near(destination[15], 4.0, 1e-6),
          "f32 square root with tail");

    double dx[5] = { 1.0, 2.0, 3.0, 4.0, 5.0 };
    double dy[5] = { 0.5, 1.0, 1.5, 2.0, 2.5 };
    double sum[5];
    simd_add_f64(sum, dx, dy, 5);
    CHECK(sum[0] == 1.5 && sum[4] == 7.5, "f64 vector add with tail");
    CHECK(simd_dot_f64(dx, dy, 5) == 27.5, "f64 array dot");
    simd_mul_f64(sum, dx, dy, 5);
    CHECK(sum[0] == 0.5 && sum[4] == 12.5, "f64 multiply with tail");
    simd_sqrt_f64(sum, dx, 5);
    CHECK(sum[0] == 1.0 && sum[3] == 2.0, "f64 square root with tail");
}

int main(void)
{
    test_math();
    test_conversion();
    test_vectors();
    printf("floattest: %d failures\n", failures);
    return failures ? 1 : 0;
}
