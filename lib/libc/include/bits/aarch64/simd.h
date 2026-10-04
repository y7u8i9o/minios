#pragma once
#include <bits/simd_types.h>

/* NEON vector operations of minios/simd.h. Included by that header; the
 * definitions apply only when compiling for this architecture. NEON is
 * part of the AArch64 baseline. */

#if defined(__aarch64__)

static inline simd_f32x4 simd_sqrt_f32x4(simd_f32x4 value)
{
    simd_f32x4 result;
    __asm__("fsqrt %0.4s, %1.4s" : "=w"(result) : "w"(value));
    return result;
}

static inline simd_f64x2 simd_sqrt_f64x2(simd_f64x2 value)
{
    simd_f64x2 result;
    __asm__("fsqrt %0.2d, %1.2d" : "=w"(result) : "w"(value));
    return result;
}

/* SSE returns the second operand for unordered or equal lanes. The lane
 * selection below reproduces that: a lane of a is chosen only where the
 * comparison is true, so a NaN in either operand and equal values give
 * the lane of b. A comparison sets every bit of a true lane. */
static inline simd_f32x4 simd_select_f32x4(simd_u32x4 mask, simd_f32x4 a, simd_f32x4 b)
{
    return (simd_f32x4)(((simd_u32x4)a & mask) | ((simd_u32x4)b & ~mask));
}

static inline simd_f64x2 simd_select_f64x2(simd_u64x2 mask, simd_f64x2 a, simd_f64x2 b)
{
    return (simd_f64x2)(((simd_u64x2)a & mask) | ((simd_u64x2)b & ~mask));
}

static inline simd_f32x4 simd_min_f32x4(simd_f32x4 a, simd_f32x4 b)
{
    return simd_select_f32x4((simd_u32x4)(a < b), a, b);
}

static inline simd_f32x4 simd_max_f32x4(simd_f32x4 a, simd_f32x4 b)
{
    return simd_select_f32x4((simd_u32x4)(a > b), a, b);
}

static inline simd_f64x2 simd_min_f64x2(simd_f64x2 a, simd_f64x2 b)
{
    return simd_select_f64x2((simd_u64x2)(a < b), a, b);
}

static inline simd_f64x2 simd_max_f64x2(simd_f64x2 a, simd_f64x2 b)
{
    return simd_select_f64x2((simd_u64x2)(a > b), a, b);
}

/* Scalar square roots without errno, for the tails of the array loops. */
static inline float simd_sqrt_f32x1(float value)
{
    float result;
    __asm__("fsqrt %s0, %s1" : "=w"(result) : "w"(value));
    return result;
}

static inline double simd_sqrt_f64x1(double value)
{
    double result;
    __asm__("fsqrt %d0, %d1" : "=w"(result) : "w"(value));
    return result;
}

#endif
