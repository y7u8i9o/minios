#pragma once
#include <bits/simd_types.h>

/* SSE2 vector operations of minios/simd.h. Included by that header; the
 * definitions apply only when compiling for this architecture. */

#if defined(__x86_64__)
#if !defined(__SSE2__)
#error "MiniOS SIMD requires the x86_64 SSE2 baseline"
#endif

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

/* Scalar square roots without errno, for the tails of the array loops. */
static inline float simd_sqrt_f32x1(float value)
{
    float result;
    __asm__("sqrtss %1, %0" : "=x"(result) : "x"(value));
    return result;
}

static inline double simd_sqrt_f64x1(double value)
{
    double result;
    __asm__("sqrtsd %1, %0" : "=x"(result) : "x"(value));
    return result;
}

#endif
