#define KLOG_SUBSYS "timer"
#include <arch/timer.h>
#include <klog.h>
#include "todo.h"
#include "timer_internal.h"

/* The clock is the virtual counter of the generic timer, which runs at
 * CNTFRQ_EL0, set by the firmware. The tick is the virtual timer of each
 * CPU (PPI 27, IRQ_TIMER), programmed one period ahead through its
 * compare value. */

#define CNTV_ENABLE (1UL << 0)

static uint64_t period;                 /* counts per tick, written once by arch_timer_init */

uint64_t arch_clock_calibrate(void)
{
    uint64_t freq;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
    return freq / 1000 ? freq / 1000 : 1;
}

void timer_rearm(void)
{
    uint64_t cval;
    __asm__ volatile("mrs %0, cntv_cval_el0" : "=r"(cval));
    cval += period;
    /* After a delay longer than a period the next tick is one period from
     * now; ticks that were missed are not delivered. */
    uint64_t now = arch_clock_read();
    if (cval <= now)
        cval = now + period;
    __asm__ volatile("msr cntv_cval_el0, %0; isb" : : "r"(cval) : "memory");
}

static void start_tick(void)
{
    uint64_t now = arch_clock_read();
    __asm__ volatile("msr cntv_cval_el0, %0; msr cntv_ctl_el0, %1; isb"
                     : : "r"(now + period), "r"(CNTV_ENABLE) : "memory");
}

void arch_timer_init(unsigned hz, uint64_t clock_per_ms)
{
    uint64_t freq = clock_per_ms * 1000;
    period = freq / hz;
    start_tick();
    klog_info("generic timer %lu Hz, periodic tick %u Hz (%lu counts) on every cpu", freq, hz, period);
}

void arch_timer_init_cpu(unsigned hz)
{
    ARCH_TODO("A8");
}
