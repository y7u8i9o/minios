#include <fenv.h>

/* The floating point environment of AArch64: the rounding mode in FPCR,
 * the exception flags in FPSR. Traps stay disabled. */

static uint64_t read_fpcr(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, fpcr" : "=r"(v));
    return v;
}

static void write_fpcr(uint64_t v)
{
    __asm__ volatile("msr fpcr, %0" : : "r"(v));
}

static uint64_t read_fpsr(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, fpsr" : "=r"(v));
    return v;
}

static void write_fpsr(uint64_t v)
{
    __asm__ volatile("msr fpsr, %0" : : "r"(v));
}

int fegetenv(fenv_t *env)
{
    env->fpcr = (uint32_t)read_fpcr();
    env->fpsr = (uint32_t)read_fpsr();
    return 0;
}

int fesetenv(const fenv_t *env)
{
    if (env == FE_DFL_ENV) {
        write_fpcr(0);
        write_fpsr(0);
        return 0;
    }
    write_fpcr(env->fpcr);
    write_fpsr(env->fpsr);
    return 0;
}

int feclearexcept(int excepts)
{
    write_fpsr(read_fpsr() & ~((uint64_t)excepts & FE_ALL_EXCEPT));
    return 0;
}

int fetestexcept(int excepts)
{
    return (int)(read_fpsr() & (unsigned)excepts & FE_ALL_EXCEPT);
}

int fegetexceptflag(fexcept_t *flag, int excepts)
{
    *flag = (fexcept_t)fetestexcept(excepts);
    return 0;
}

int fesetexceptflag(const fexcept_t *flag, int excepts)
{
    uint64_t mask = (unsigned)excepts & FE_ALL_EXCEPT;
    write_fpsr((read_fpsr() & ~mask) | ((uint64_t)*flag & mask));
    return 0;
}

int feraiseexcept(int excepts)
{
    write_fpsr(read_fpsr() | ((unsigned)excepts & FE_ALL_EXCEPT));
    return 0;
}

int fegetround(void)
{
    return (int)(read_fpcr() & FE_RMODE_MASK);
}

int __flt_rounds(void)
{
    switch (fegetround()) {
    case FE_TOWARDZERO: return 0;
    case FE_TONEAREST: return 1;
    case FE_UPWARD: return 2;
    case FE_DOWNWARD: return 3;
    default: return -1;
    }
}

int fesetround(int mode)
{
    if (mode != FE_TONEAREST && mode != FE_DOWNWARD &&
        mode != FE_UPWARD && mode != FE_TOWARDZERO)
        return -1;
    write_fpcr((read_fpcr() & ~(uint64_t)FE_RMODE_MASK) | (unsigned)mode);
    return 0;
}

int feholdexcept(fenv_t *env)
{
    fegetenv(env);
    write_fpsr(read_fpsr() & ~(uint64_t)FE_ALL_EXCEPT);
    return 0;
}

int feupdateenv(const fenv_t *env)
{
    int raised = fetestexcept(FE_ALL_EXCEPT);
    fesetenv(env);
    return feraiseexcept(raised);
}
