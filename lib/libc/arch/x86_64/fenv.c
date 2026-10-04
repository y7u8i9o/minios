#include <fenv.h>

#define X87_ALL_FLAGS 0x003fU
#define MXCSR_ALL_FLAGS 0x0000003fU
#define MXCSR_ALL_MASKS 0x00001f80U
#define MXCSR_ROUND_MASK 0x00006000U

static void environment_read(fenv_t *env)
{
    /* FNSTENV masks x87 traps as a side effect, so reload the stored image
     * before returning. */
    __asm__ volatile("fnstenv %0" : "=m" (env->x87));
    __asm__ volatile("fldenv %0" : : "m" (env->x87));
    __asm__ volatile("stmxcsr %0" : "=m" (env->mxcsr));
}

static void environment_write(const fenv_t *env)
{
    __asm__ volatile("fldenv %0" : : "m" (env->x87));
    __asm__ volatile("ldmxcsr %0" : : "m" (env->mxcsr));
}

int fegetenv(fenv_t *env)
{
    environment_read(env);
    return 0;
}

int fesetenv(const fenv_t *env)
{
    if (env == FE_DFL_ENV) {
        uint32_t mxcsr = 0x1f80U;
        __asm__ volatile("fninit");
        __asm__ volatile("ldmxcsr %0" : : "m" (mxcsr));
        return 0;
    }
    environment_write(env);
    return 0;
}

int feclearexcept(int excepts)
{
    fenv_t env;
    unsigned flags = (unsigned)excepts & FE_ALL_EXCEPT;
    environment_read(&env);
    env.x87.status &= (uint16_t)~flags;
    env.mxcsr &= ~flags;
    environment_write(&env);
    return 0;
}

int fetestexcept(int excepts)
{
    uint16_t x87_status;
    uint32_t mxcsr;
    __asm__ volatile("fnstsw %0" : "=am" (x87_status));
    __asm__ volatile("stmxcsr %0" : "=m" (mxcsr));
    return (int)((x87_status | mxcsr) & (unsigned)excepts & FE_ALL_EXCEPT);
}

int fegetexceptflag(fexcept_t *flag, int excepts)
{
    *flag = (fexcept_t)fetestexcept(excepts);
    return 0;
}

int fesetexceptflag(const fexcept_t *flag, int excepts)
{
    fenv_t env;
    unsigned mask = (unsigned)excepts & FE_ALL_EXCEPT;
    unsigned value = (unsigned)*flag & mask;
    environment_read(&env);
    env.x87.status = (uint16_t)((env.x87.status & ~mask) | value);
    env.mxcsr = (env.mxcsr & ~mask) | value;
    environment_write(&env);
    return 0;
}

int feraiseexcept(int excepts)
{
    fenv_t env;
    unsigned flags = (unsigned)excepts & FE_ALL_EXCEPT;
    environment_read(&env);
    env.x87.status |= (uint16_t)flags;
    env.mxcsr |= flags;
    environment_write(&env);
    return 0;
}

int fegetround(void)
{
    uint16_t control;
    __asm__ volatile("fnstcw %0" : "=m" (control));
    return control & 0x0c00U;
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
    uint16_t control;
    uint32_t mxcsr;
    __asm__ volatile("fnstcw %0" : "=m" (control));
    __asm__ volatile("stmxcsr %0" : "=m" (mxcsr));
    control = (uint16_t)((control & ~0x0c00U) | (unsigned)mode);
    mxcsr = (mxcsr & ~MXCSR_ROUND_MASK) | ((unsigned)mode << 3);
    __asm__ volatile("fldcw %0" : : "m" (control));
    __asm__ volatile("ldmxcsr %0" : : "m" (mxcsr));
    return 0;
}

int feholdexcept(fenv_t *env)
{
    fenv_t saved;
    environment_read(env);
    saved = *env;
    saved.x87.control |= X87_ALL_FLAGS;
    saved.x87.status &= (uint16_t)~X87_ALL_FLAGS;
    saved.mxcsr = (saved.mxcsr & ~MXCSR_ALL_FLAGS) | MXCSR_ALL_MASKS;
    environment_write(&saved);
    return 0;
}

int feupdateenv(const fenv_t *env)
{
    int raised = fetestexcept(FE_ALL_EXCEPT);
    fesetenv(env);
    return feraiseexcept(raised);
}
