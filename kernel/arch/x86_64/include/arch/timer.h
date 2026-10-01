#pragma once
#include <kernel.h>

/* The monotonic clock and the per CPU tick of the architecture
 * (docs/design/arch.md). drivers/timer.c converts the clock to
 * nanoseconds and runs the tick handlers. */

/* Free running counter of the clock: the TSC. */
static inline uint64_t arch_clock_read(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Counts of arch_clock_read per millisecond, at least 1. Measures the TSC
 * against the PIT over 20 ms; called once, first thing in kmain. */
uint64_t arch_clock_calibrate(void);
/* Log the clock and start the periodic tick of the boot CPU at hz on
 * IRQ_TIMER. */
void arch_timer_init(unsigned hz, uint64_t clock_per_ms);
/* Start the tick of an application processor. */
void arch_timer_init_cpu(unsigned hz);
