/* M28: extended libm, hexadecimal conversion and packed SSE2 operations. */
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
        printf("mathvectest: FAIL " __VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

static int close_relative(double actual, double expected, double tolerance)
{
    double scale = fmax(1.0, fabs(expected));
    return fabs(actual - expected) <= tolerance * scale;
}

static void test_round_trips(void)
{
    static const double logarithm_inputs[] = {
        0x1p-100, 0.125, 0.5, 1.0, 3.0, 10.0, 0x1p100
    };
    for (unsigned i = 0; i < sizeof(logarithm_inputs) / sizeof(logarithm_inputs[0]); i++) {
        double value = logarithm_inputs[i];
        CHECK(close_relative(exp(log(value)), value, 3e-14),
              "exp log round trip %u", i);
    }

    static const double angles[] = {
        -10.0, -M_PI, -M_PI_4, 0.0, M_PI / 6.0, M_PI_2, 10.0
    };
    for (unsigned i = 0; i < sizeof(angles) / sizeof(angles[0]); i++) {
        double sine = sin(angles[i]), cosine = cos(angles[i]);
        CHECK(close_relative(sine * sine + cosine * cosine, 1.0, 3e-15),
              "trigonometric identity %u", i);
    }
    CHECK(close_relative(pow(7.0, 3.5), exp(3.5 * log(7.0)), 2e-14),
          "pow exponential identity");
    CHECK(close_relative(cbrt(1e-300), 1e-100, 3e-14), "cbrt small magnitude");
}

static void test_conversion(void)
{
    char *end;
    CHECK(strtod("0x1.fffffffffffffp+1023!", &end) == DBL_MAX && *end == '!',
          "largest hexadecimal double");
    CHECK(strtod("0x0.0000000000001p-1022", &end) == DBL_TRUE_MIN && *end == '\0',
          "smallest hexadecimal double");
    CHECK(strtof("0x1.fffffep+127", &end) == FLT_MAX && *end == '\0',
          "largest hexadecimal float");
    errno = 0;
    CHECK(strtof("0x1p-150", NULL) == 0.0f && errno == ERANGE,
          "float hexadecimal underflow");
}

static void test_vector_pipeline(void)
{
    float a[11], b[11], product[11], scaled[11], root[11];
    for (int i = 0; i < 11; i++) {
        a[i] = (float)(i + 1);
        b[i] = (float)(i + 1);
    }
    simd_mul_f32(product, a, b, 11);
    simd_scale_f32(scaled, product, 4.0f, 11);
    simd_sqrt_f32(root, scaled, 11);
    for (int i = 0; i < 11; i++)
        CHECK(root[i] == (float)(2 * (i + 1)), "vector pipeline lane %d", i);

    simd_f64x2 value = simd_set_f64x2(-9.0, 100.0);
    simd_f64x2 root64 = simd_sqrt_f64x2(simd_abs_f64x2(value));
    CHECK(root64[0] == 3.0 && root64[1] == 10.0, "packed double square root");
    simd_f64x2 limited = simd_clamp_f64x2(value, simd_splat_f64x2(-4.0),
                                          simd_splat_f64x2(4.0));
    CHECK(limited[0] == -4.0 && limited[1] == 4.0, "packed double clamp");
}

int main(void)
{
    test_round_trips();
    test_conversion();
    test_vector_pipeline();
    printf("mathvectest: %d failures\n", failures);
    return failures ? 1 : 0;
}
