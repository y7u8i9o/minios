#pragma once
#include <stdint.h>

/* Native 128-bit vector types of minios/simd.h. They may be stored in vector
 * registers across a scheduling point because the kernel saves the
 * complete FP and SIMD state of a thread. */
typedef float simd_f32x4 __attribute__((vector_size(16)));
typedef double simd_f64x2 __attribute__((vector_size(16)));
typedef int32_t simd_i32x4 __attribute__((vector_size(16)));
typedef uint32_t simd_u32x4 __attribute__((vector_size(16)));
typedef uint64_t simd_u64x2 __attribute__((vector_size(16)));
