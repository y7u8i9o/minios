#include <minios/simd.h>

void simd_add_f32(float *restrict destination, const float *restrict a,
                  const float *restrict b, size_t count)
{
    size_t i = 0;
    for (; i + 4 <= count; i += 4)
        simd_store_f32x4(destination + i, simd_load_f32x4(a + i) + simd_load_f32x4(b + i));
    for (; i < count; i++)
        destination[i] = a[i] + b[i];
}

void simd_add_f64(double *restrict destination, const double *restrict a,
                  const double *restrict b, size_t count)
{
    size_t i = 0;
    for (; i + 2 <= count; i += 2)
        simd_store_f64x2(destination + i, simd_load_f64x2(a + i) + simd_load_f64x2(b + i));
    for (; i < count; i++)
        destination[i] = a[i] + b[i];
}

void simd_mul_f32(float *restrict destination, const float *restrict a,
                  const float *restrict b, size_t count)
{
    size_t i = 0;
    for (; i + 4 <= count; i += 4)
        simd_store_f32x4(destination + i, simd_load_f32x4(a + i) * simd_load_f32x4(b + i));
    for (; i < count; i++)
        destination[i] = a[i] * b[i];
}

void simd_mul_f64(double *restrict destination, const double *restrict a,
                  const double *restrict b, size_t count)
{
    size_t i = 0;
    for (; i + 2 <= count; i += 2)
        simd_store_f64x2(destination + i, simd_load_f64x2(a + i) * simd_load_f64x2(b + i));
    for (; i < count; i++)
        destination[i] = a[i] * b[i];
}

void simd_scale_f32(float *restrict destination, const float *restrict source,
                    float scale, size_t count)
{
    simd_f32x4 vector_scale = simd_splat_f32x4(scale);
    size_t i = 0;
    for (; i + 4 <= count; i += 4)
        simd_store_f32x4(destination + i, simd_load_f32x4(source + i) * vector_scale);
    for (; i < count; i++)
        destination[i] = source[i] * scale;
}

void simd_scale_f64(double *restrict destination, const double *restrict source,
                    double scale, size_t count)
{
    simd_f64x2 vector_scale = simd_splat_f64x2(scale);
    size_t i = 0;
    for (; i + 2 <= count; i += 2)
        simd_store_f64x2(destination + i, simd_load_f64x2(source + i) * vector_scale);
    for (; i < count; i++)
        destination[i] = source[i] * scale;
}

void simd_sqrt_f32(float *restrict destination, const float *restrict source,
                   size_t count)
{
    size_t i = 0;
    for (; i + 4 <= count; i += 4)
        simd_store_f32x4(destination + i, simd_sqrt_f32x4(simd_load_f32x4(source + i)));
    for (; i < count; i++) {
        destination[i] = simd_sqrt_f32x1(source[i]);
    }
}

void simd_sqrt_f64(double *restrict destination, const double *restrict source,
                   size_t count)
{
    size_t i = 0;
    for (; i + 2 <= count; i += 2)
        simd_store_f64x2(destination + i, simd_sqrt_f64x2(simd_load_f64x2(source + i)));
    for (; i < count; i++) {
        destination[i] = simd_sqrt_f64x1(source[i]);
    }
}

void simd_saxpy_f32(float *restrict destination, const float *restrict x,
                    const float *restrict y, float alpha, size_t count)
{
    simd_f32x4 scale = simd_splat_f32x4(alpha);
    size_t i = 0;
    for (; i + 4 <= count; i += 4) {
        simd_f32x4 result = scale * simd_load_f32x4(x + i) + simd_load_f32x4(y + i);
        simd_store_f32x4(destination + i, result);
    }
    for (; i < count; i++)
        destination[i] = alpha * x[i] + y[i];
}

float simd_dot_f32(const float *a, const float *b, size_t count)
{
    simd_f32x4 sum = simd_splat_f32x4(0.0f);
    size_t i = 0;
    for (; i + 4 <= count; i += 4)
        sum += simd_load_f32x4(a + i) * simd_load_f32x4(b + i);
    float result = simd_sum_f32x4(sum);
    for (; i < count; i++)
        result += a[i] * b[i];
    return result;
}

double simd_dot_f64(const double *a, const double *b, size_t count)
{
    simd_f64x2 sum = simd_splat_f64x2(0.0);
    size_t i = 0;
    for (; i + 2 <= count; i += 2)
        sum += simd_load_f64x2(a + i) * simd_load_f64x2(b + i);
    double result = simd_sum_f64x2(sum);
    for (; i < count; i++)
        result += a[i] * b[i];
    return result;
}
