#pragma once

#include <stdint.h>

/* The exception flags of FPSR. */
#define FE_INVALID    0x01
#define FE_DIVBYZERO  0x02
#define FE_OVERFLOW   0x04
#define FE_UNDERFLOW  0x08
#define FE_INEXACT    0x10
#define FE_ALL_EXCEPT (FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW | \
                       FE_UNDERFLOW | FE_INEXACT)

/* The rounding mode field of FPCR (bits 23:22). */
#define FE_TONEAREST  0x000000
#define FE_UPWARD     0x400000
#define FE_DOWNWARD   0x800000
#define FE_TOWARDZERO 0xc00000
#define FE_RMODE_MASK 0xc00000

typedef unsigned int fexcept_t;

typedef struct {
    uint32_t fpcr;
    uint32_t fpsr;
} fenv_t;

#define FE_DFL_ENV ((const fenv_t *)-1L)
