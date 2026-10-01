#pragma once
#include <kernel.h>

/* The monotonic clock and the per CPU tick (docs/design/arch.md): the
 * virtual counter and the virtual timer of the generic timer. */

static inline uint64_t arch_clock_read(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(v) : : "memory");
    return v;
}

uint64_t arch_clock_calibrate(void);
void arch_timer_init(unsigned hz, uint64_t clock_per_ms);
void arch_timer_init_cpu(unsigned hz);
