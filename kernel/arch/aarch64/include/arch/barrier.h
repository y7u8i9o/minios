#pragma once

/* Memory barriers and the spin wait hint (docs/design/arch.md). Kernel data
 * shared between CPUs is ordered with the __atomic builtins; these order
 * normal memory against device accesses. The full system domain covers
 * devices and other CPUs. */

static inline void mb(void)
{
    __asm__ volatile("dsb sy" : : : "memory");
}

static inline void rmb(void)
{
    __asm__ volatile("dsb ld" : : : "memory");
}

static inline void wmb(void)
{
    __asm__ volatile("dsb st" : : : "memory");
}

static inline void cpu_relax(void)
{
    __asm__ volatile("yield" : : : "memory");
}
