#define KLOG_SUBSYS "timer"
#include <arch/timer.h>
#include <arch/apic.h>
#include <arch/cpu.h>
#include <arch/pit.h>
#include <klog.h>

/* Time is read from the TSC, which QEMU derives from host time, so it does
 * not lose accuracy when timer interrupts are delayed or dropped under
 * emulation. Every CPU has its own local APIC timer for the tick. */

uint64_t arch_clock_calibrate(void)
{
    uint64_t start = arch_clock_read();
    pit_wait_us(20000);
    uint64_t per_ms = (arch_clock_read() - start) / 20;
    return per_ms ? per_ms : 1;
}

void arch_timer_init(unsigned hz, uint64_t clock_per_ms)
{
    klog_info("tsc %lu.%03lu MHz, measured against the pit over 20 ms, invariant %s",
              clock_per_ms / 1000, clock_per_ms % 1000, cpu_features.invariant_tsc ? "yes" : "no");
    uint64_t lapic_hz = lapic_timer_calibrate();
    lapic_timer_start(hz);
    klog_info("lapic timer %lu Hz at divider 16, periodic tick %u Hz (%lu counts) on every cpu",
              lapic_hz, hz, lapic_hz / hz);
}

void arch_timer_init_cpu(unsigned hz)
{
    lapic_timer_start(hz);
}
