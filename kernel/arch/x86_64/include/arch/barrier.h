#pragma once

/* Memory barriers and the spin wait hint (docs/design/arch.md).
 *
 * Kernel data shared between CPUs is ordered with the __atomic builtins;
 * these barriers order normal memory against device accesses, such as a
 * virtqueue update against the notification that publishes it. On x86 they
 * map to the fence instructions; under total store order mb is the only
 * one that changes what the processor does. */

/* Full barrier: no load or store crosses it. */
static inline void mb(void)
{
    __asm__ volatile("mfence" : : : "memory");
}

/* Loads before the barrier complete before loads after it. */
static inline void rmb(void)
{
    __asm__ volatile("lfence" : : : "memory");
}

/* Stores before the barrier are visible before stores after it. */
static inline void wmb(void)
{
    __asm__ volatile("sfence" : : : "memory");
}

/* Hint inside a spin wait loop. */
static inline void cpu_relax(void)
{
    __asm__ volatile("pause");
}
