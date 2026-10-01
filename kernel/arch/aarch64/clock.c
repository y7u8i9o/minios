#include <arch/timer.h>
#include "todo.h"

/* The virtual counter runs at CNTFRQ_EL0, which the firmware sets. */
uint64_t arch_clock_calibrate(void)
{
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    return freq / 1000 ? freq / 1000 : 1;
}

void arch_timer_init(unsigned hz, uint64_t clock_per_ms)
{
    ARCH_TODO("A5");
}

void arch_timer_init_cpu(unsigned hz)
{
    ARCH_TODO("A8");
}
