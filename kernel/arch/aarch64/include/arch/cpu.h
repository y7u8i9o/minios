#pragma once
#include <kernel.h>
#include <cpu.h>
#include <arch/barrier.h>

/* The aarch64 CPU primitives (docs/design/arch.md). struct cpu is reached
 * through TPIDR_EL1, which contains its address on every CPU. */

#define DAIF_I (1UL << 7)       /* IRQ mask bit of DAIF */

static inline struct cpu *cpu_current(void)
{
    struct cpu *c;
    __asm__ volatile("mrs %0, tpidr_el1" : "=r"(c));
    return c;
}

static inline uint64_t read_daif(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, daif" : "=r"(v));
    return v;
}

/* Interrupt state for generic code. Interrupt disabling is never mutual
 * exclusion on its own (docs/design/locking.md). arch_irq_save masks IRQs
 * and returns the previous state for arch_irq_restore. */
static inline void arch_irq_enable(void)
{
    __asm__ volatile("msr daifclr, #2" : : : "memory");
}

static inline void arch_irq_disable(void)
{
    __asm__ volatile("msr daifset, #2" : : : "memory");
}

static inline bool arch_irqs_enabled(void)
{
    return !(read_daif() & DAIF_I);
}

/* True if the state saved by arch_irq_save had interrupts enabled. */
static inline bool arch_irq_flags_enabled(unsigned long flags)
{
    return !(flags & DAIF_I);
}

static inline unsigned long arch_irq_save(void)
{
    unsigned long flags = read_daif();
    arch_irq_disable();
    return flags;
}

static inline void arch_irq_restore(unsigned long flags)
{
    if (!(flags & DAIF_I))
        arch_irq_enable();
}

/* Wait for the next interrupt with the current interrupt state. */
static inline void arch_wait_for_interrupt(void)
{
    __asm__ volatile("wfi" : : : "memory");
}

/* Wait for the next interrupt, then take it (the idle loop). A pending
 * interrupt ends wfi even while IRQs are masked, so masking first and
 * unmasking after wfi cannot lose a wakeup. */
static inline void arch_idle(void)
{
    __asm__ volatile("msr daifset, #2; wfi; msr daifclr, #2; isb" : : : "memory");
}

static inline __noreturn void cpu_halt_forever(void)
{
    for (;;)
        __asm__ volatile("msr daifset, #0xf; wfi" : : : "memory");
}

/* Free running counter for lock statistics and the profiler: the virtual
 * counter of the generic timer. */
static inline uint64_t arch_cycles(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v) : : "memory");
    return v;
}

/* Set up the boot CPU structure and point TPIDR_EL1 at it. */
void cpu_init_boot(void);
/* FP and SIMD access and EL0 cache maintenance on the calling CPU. */
void cpu_init_el0_access(void);
