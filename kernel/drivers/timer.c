#define KLOG_SUBSYS "timer"
#include <ipc/alarm.h>
#include <drivers/timer.h>
#include <ipc/eventfd.h>
#include <sched/wait.h>
#include <arch/irq.h>
#include <arch/cpu.h>
#include <arch/timer.h>
#include <klog.h>
#include <errno.h>
#include <sched/sched.h>
#include <sched/proc.h>
#include <debug/profile.h>

/* Time is read from the clock of the architecture (arch_clock_read, the
 * TSC on x86_64). clock_base and clock_per_ms are written once by
 * timer_early_init and read only afterwards.
 *
 * Every CPU has its own tick at TIMER_HZ. Each tick runs the scheduler work
 * of its CPU: sched_tick on the boot CPU through the tick handler, and
 * sched_tick_cpu on the other CPUs. The boot CPU also counts the ticks and
 * runs the timer file descriptors, the alarms and the wait timeouts. */
static volatile uint64_t ticks;             /* boot CPU interrupts, statistics only */
static uint64_t clock_base;
static uint64_t clock_per_ms;
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
    alarm_tick();
    waitq_timeouts_tick();
}

/* Calibrate the clock. Runs first thing in kmain so that timer_ns counts
 * from the kernel entry and the log can stamp every line, including those
 * of the early console and memory setup. */
void timer_early_init(void)
{
    clock_base = arch_clock_read();
    clock_per_ms = arch_clock_calibrate();
}

void timer_init(void)
{
    irq_register(IRQ_TIMER, timer_irq, NULL);
    arch_timer_init(TIMER_HZ, clock_per_ms);
}

void timer_init_cpu(void)
{
    arch_timer_init_cpu(TIMER_HZ);
}

uint64_t timer_clock_hz(void)
{
    return clock_per_ms * 1000;
}

uint64_t timer_ms(void)
{
    if (!clock_per_ms)
        return 0;
    return (arch_clock_read() - clock_base) / clock_per_ms;
}

uint64_t timer_ns(void)
{
    if (!clock_per_ms)
        return 0;
    uint64_t t = arch_clock_read() - clock_base;
    /* Whole milliseconds, then the remainder scaled without overflow. */
    return (t / clock_per_ms) * 1000000 + (t % clock_per_ms) * 1000000 / clock_per_ms;
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

/* sleep_until_tick sleeps until the tick.  A wake before the tick, such as
 * a stale wake of the scheduler, starts another sleep
 * (docs/postmortems/2026-10-05-sleep-wakeup.md).  The sleep ends early
 * only when interrupted returns true; the result is then -EINTR. */
static int sleep_until_tick(uint64_t until, bool (*interrupted)(void))
{
    if (!sched_started()) {
        /* Busy variant for early boot. */
        while (timer_ticks() < until) {
            if (arch_irqs_enabled())
                arch_wait_for_interrupt();
            else
                cpu_relax();
        }
        return 0;
    }
    while (timer_ticks() < until) {
        if (!sched_sleep_until(until, interrupted))
            return -EINTR;
        if (interrupted && timer_ticks() < until && interrupted())
            return -EINTR;
    }
    return 0;
}

/* The start lies somewhere inside the current tick; counting from the next
 * tick boundary makes the sleep last at least ms. */
static uint64_t deadline_tick(uint64_t ms)
{
    return (timer_ns() + 999999) / 1000000 * (TIMER_HZ / 1000) + ms * (TIMER_HZ / 1000);
}

void sleep_ms(uint64_t ms)
{
    sleep_until_tick(deadline_tick(ms), NULL);
}

int sleep_ms_interruptible(uint64_t ms, bool (*interrupted)(void))
{
    return sleep_until_tick(deadline_tick(ms), interrupted);
}
