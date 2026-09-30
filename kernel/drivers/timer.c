#define KLOG_SUBSYS "timer"
#include <drivers/timer.h>
#include <ipc/eventfd.h>
#include <sched/wait.h>
#include <arch/apic.h>
#include <arch/irq.h>
#include <arch/cpu.h>
#include <arch/pit.h>
#include <klog.h>
#include <sched/sched.h>
#include <sched/proc.h>
#include <debug/profile.h>

/* Time is read from the TSC, which QEMU derives from host time, so it does
 * not lose accuracy when timer interrupts are delayed or dropped under
 * emulation. tsc_base and tsc_per_ms are written once by timer_init and
 * read only afterwards.
 *
 * Every CPU has its own local APIC timer at TIMER_HZ. The boot CPU's
 * interrupt counts ticks and runs the tick handler (sleepers and the
 * scheduler boost), the other CPUs account their running thread's slice
 * through sched_tick_cpu. */
static volatile uint64_t ticks;             /* boot CPU interrupts, statistics only */
static uint64_t tsc_base;
static uint64_t tsc_per_ms;
static timer_tick_fn tick_handler;

static void timer_irq(struct trapframe *tf, void *arg)
{
    struct cpu *c = cpu_current();
    c->ticks++;
    if (sched_started()) {
        proc_account_tick(tf);
        profile_sample(tf);
        profile_tick();
    }
    if (c->id != 0) {
        sched_tick_cpu();
        return;
    }
    ticks++;
    if (tick_handler)
        tick_handler();
    timerfd_tick();
    waitq_timeouts_tick();
}

/* Measure the TSC against the PIT over 20 ms. Runs first thing in kmain
 * so that timer_ns counts from the kernel entry and the log can stamp
 * every line, including those of the early console and memory setup. */
void timer_early_init(void)
{
    tsc_base = rdtsc();
    pit_wait_us(20000);
    tsc_per_ms = (rdtsc() - tsc_base) / 20;
    if (tsc_per_ms == 0)
        tsc_per_ms = 1;
}

void timer_init(void)
{
    irq_register(IRQ_TIMER, timer_irq, NULL);
    klog_info("tsc %lu.%03lu MHz, measured against the pit over 20 ms, invariant %s",
              tsc_per_ms / 1000, tsc_per_ms % 1000, cpu_features.invariant_tsc ? "yes" : "no");
    uint64_t lapic_hz = lapic_timer_calibrate();
    lapic_timer_start(TIMER_HZ);
    klog_info("lapic timer %lu Hz at divider 16, periodic tick %u Hz (%lu counts) on every cpu",
              lapic_hz, TIMER_HZ, lapic_hz / TIMER_HZ);
}

void timer_init_cpu(void)
{
    lapic_timer_start(TIMER_HZ);
}

uint64_t timer_ms(void)
{
    if (!tsc_per_ms)
        return 0;
    return (rdtsc() - tsc_base) / tsc_per_ms;
}

uint64_t timer_ns(void)
{
    if (!tsc_per_ms)
        return 0;
    uint64_t t = rdtsc() - tsc_base;
    /* Whole milliseconds, then the remainder scaled without overflow. */
    return (t / tsc_per_ms) * 1000000 + (t % tsc_per_ms) * 1000000 / tsc_per_ms;
}

uint64_t timer_ticks(void)
{
    return timer_ms() * (TIMER_HZ / 1000);
}

uint64_t timer_interrupts(void)
{
    return ticks;
}

void timer_set_tick_handler(timer_tick_fn fn)
{
    tick_handler = fn;
}

void sleep_ms(uint64_t ms)
{
    /* The start lies somewhere inside the current tick; counting from the
     * next tick boundary makes the sleep last at least ms. */
    uint64_t until = (timer_ns() + 999999) / 1000000 * (TIMER_HZ / 1000) + ms * (TIMER_HZ / 1000);
    if (sched_started()) {
        sched_sleep_until(until);
        return;
    }
    /* Busy variant for early boot. */
    while (timer_ticks() < until) {
        if (read_rflags() & RFLAGS_IF)
            hlt();
        else
            cpu_relax();
    }
}
