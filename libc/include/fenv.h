#pragma once

#include <stdint.h>

/* x87 and MXCSR use the same six low exception-status bits. */
#define FE_INVALID    0x01
#define FE_DIVBYZERO  0x04
#define FE_OVERFLOW   0x08
#define FE_UNDERFLOW  0x10
#define FE_INEXACT    0x20
#define FE_ALL_EXCEPT (FE_INVALID | FE_DIVBYZERO | FE_OVERFLOW | \
                       FE_UNDERFLOW | FE_INEXACT)

/* These values are the x87 control-word rounding field. */
#define FE_TONEAREST  0x0000
#define FE_DOWNWARD   0x0400
#define FE_UPWARD     0x0800
#define FE_TOWARDZERO 0x0c00

typedef unsigned short fexcept_t;

struct __fenv_x87 {
    uint16_t control;
    uint16_t reserved0;
    uint16_t status;
    uint16_t reserved1;
    uint16_t tag;
    uint16_t reserved2;
    uint32_t instruction_pointer;
    uint16_t code_segment;
    uint16_t opcode;
    uint32_t data_pointer;
    uint16_t data_segment;
    uint16_t reserved3;
};

typedef struct {
    struct __fenv_x87 x87;
    uint32_t mxcsr;
} fenv_t;

#define FE_DFL_ENV ((const fenv_t *)-1L)

int feclearexcept(int excepts);
int fegetexceptflag(fexcept_t *flag, int excepts);
int feraiseexcept(int excepts);
int fesetexceptflag(const fexcept_t *flag, int excepts);
int fetestexcept(int excepts);
int fegetround(void);
int fesetround(int mode);
int fegetenv(fenv_t *env);
int feholdexcept(fenv_t *env);
int fesetenv(const fenv_t *env);
int feupdateenv(const fenv_t *env);
