#pragma once
#include <stddef.h>
#include <stdint.h>

#if !defined(__SSE2__)
#error "MiniOS SIMD requires the x86_64 SSE2 baseline"
#endif

/* Native 128-bit vector types. They may be kept in XMM registers across a
 * scheduling point because the kernel saves the complete FXSAVE area. */
typedef float simd_f32x4 __attribute__((vector_size(16)));
typedef double simd_f64x2 __attribute__((vector_size(16)));
typedef int32_t simd_i32x4 __attribute__((vector_size(16)));
typedef uint32_t simd_u32x4 __attribute__((vector_size(16)));
typedef uint64_t simd_u64x2 __attribute__((vector_size(16)));

static inline simd_f32x4 simd_set_f32x4(float x0, float x1, float x2, float x3)
{
    return (simd_f32x4){ x0, x1, x2, x3 };
}

static inline simd_f64x2 simd_set_f64x2(double x0, double x1)
{
    return (simd_f64x2){ x0, x1 };
}

static inline simd_f32x4 simd_splat_f32x4(float value)
{
    return (simd_f32x4){ value, value, value, value };
}

static inline simd_f64x2 simd_splat_f64x2(double value)
{
    return (simd_f64x2){ value, value };
}

/* These helpers deliberately accept unaligned addresses. Explicit memcpy
 * builtins let GCC select movups/movupd without violating C aliasing rules. */
static inline simd_f32x4 simd_load_f32x4(const float *source)
{
    simd_f32x4 value;
    __builtin_memcpy(&value, source, sizeof(value));
    return value;
}

static inline simd_f64x2 simd_load_f64x2(const double *source)
{
    simd_f64x2 value;
    __builtin_memcpy(&value, source, sizeof(value));
    return value;
}

static inline void simd_store_f32x4(float *destination, simd_f32x4 value)
{
    __builtin_memcpy(destination, &value, sizeof(value));
}

static inline void simd_store_f64x2(double *destination, simd_f64x2 value)
{
    __builtin_memcpy(destination, &value, sizeof(value));
}

static inline simd_f32x4 simd_add_f32x4(simd_f32x4 a, simd_f32x4 b) { return a + b; }
static inline simd_f32x4 simd_sub_f32x4(simd_f32x4 a, simd_f32x4 b) { return a - b; }
static inline simd_f32x4 simd_mul_f32x4(simd_f32x4 a, simd_f32x4 b) { return a * b; }
static inline simd_f32x4 simd_div_f32x4(simd_f32x4 a, simd_f32x4 b) { return a / b; }
static inline simd_f64x2 simd_add_f64x2(simd_f64x2 a, simd_f64x2 b) { return a + b; }
static inline simd_f64x2 simd_sub_f64x2(simd_f64x2 a, simd_f64x2 b) { return a - b; }
static inline simd_f64x2 simd_mul_f64x2(simd_f64x2 a, simd_f64x2 b) { return a * b; }
static inline simd_f64x2 simd_div_f64x2(simd_f64x2 a, simd_f64x2 b) { return a / b; }

static inline simd_f32x4 simd_abs_f32x4(simd_f32x4 value)
{
    union { simd_f32x4 floating; simd_u32x4 integer; } bits = { .floating = value };
    bits.integer &= (simd_u32x4){ 0x7fffffffU, 0x7fffffffU, 0x7fffffffU, 0x7fffffffU };
    return bits.floating;
}

static inline simd_f64x2 simd_abs_f64x2(simd_f64x2 value)
{
    union { simd_f64x2 floating; simd_u64x2 integer; } bits = { .floating = value };
    bits.integer &= (simd_u64x2){ 0x7fffffffffffffffULL, 0x7fffffffffffffffULL };
    return bits.floating;
}

static inline simd_f32x4 simd_sqrt_f32x4(simd_f32x4 value)
{
    simd_f32x4 result;
    __asm__("sqrtps %1, %0" : "=x"(result) : "x"(value));
    return result;
}

static inline simd_f64x2 simd_sqrt_f64x2(simd_f64x2 value)
{
    simd_f64x2 result;
    __asm__("sqrtpd %1, %0" : "=x"(result) : "x"(value));
    return result;
}

/* minps/maxps and minpd/maxpd return their second operand for unordered or
 * equal lanes. This is the native SSE behavior, not the scalar fmin/fmax NaN
 * and signed-zero policy. */
static inline simd_f32x4 simd_min_f32x4(simd_f32x4 a, simd_f32x4 b)
{
    __asm__("minps %1, %0" : "+x"(a) : "x"(b));
    return a;
}

static inline simd_f32x4 simd_max_f32x4(simd_f32x4 a, simd_f32x4 b)
{
    __asm__("maxps %1, %0" : "+x"(a) : "x"(b));
    return a;
}

static inline simd_f64x2 simd_min_f64x2(simd_f64x2 a, simd_f64x2 b)
{
    __asm__("minpd %1, %0" : "+x"(a) : "x"(b));
    return a;
}

static inline simd_f64x2 simd_max_f64x2(simd_f64x2 a, simd_f64x2 b)
{
    __asm__("maxpd %1, %0" : "+x"(a) : "x"(b));
    return a;
}

static inline simd_f32x4 simd_clamp_f32x4(simd_f32x4 value,
                                           simd_f32x4 lower, simd_f32x4 upper)
{
    return simd_min_f32x4(simd_max_f32x4(value, lower), upper);
}

static inline simd_f64x2 simd_clamp_f64x2(simd_f64x2 value,
                                           simd_f64x2 lower, simd_f64x2 upper)
{
    return simd_min_f64x2(simd_max_f64x2(value, lower), upper);
}

static inline float simd_sum_f32x4(simd_f32x4 value)
{
    return value[0] + value[1] + value[2] + value[3];
}

static inline double simd_sum_f64x2(simd_f64x2 value)
{
    return value[0] + value[1];
}

static inline float simd_dot_f32x4(simd_f32x4 a, simd_f32x4 b)
{
    return simd_sum_f32x4(a * b);
}

static inline double simd_dot_f64x2(simd_f64x2 a, simd_f64x2 b)
{
    return simd_sum_f64x2(a * b);
}

void simd_add_f32(float *restrict destination, const float *restrict a,
                  const float *restrict b, size_t count);
void simd_add_f64(double *restrict destination, const double *restrict a,
                  const double *restrict b, size_t count);
void simd_mul_f32(float *restrict destination, const float *restrict a,
                  const float *restrict b, size_t count);
void simd_mul_f64(double *restrict destination, const double *restrict a,
                  const double *restrict b, size_t count);
void simd_scale_f32(float *restrict destination, const float *restrict source,
                    float scale, size_t count);
void simd_scale_f64(double *restrict destination, const double *restrict source,
                    double scale, size_t count);
void simd_sqrt_f32(float *restrict destination, const float *restrict source,
                   size_t count);
void simd_sqrt_f64(double *restrict destination, const double *restrict source,
                   size_t count);
void simd_saxpy_f32(float *restrict destination, const float *restrict x,
                    const float *restrict y, float alpha, size_t count);
float simd_dot_f32(const float *a, const float *b, size_t count);
double simd_dot_f64(const double *a, const double *b, size_t count);
