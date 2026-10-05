#pragma once
#include <kernel.h>

#define TIMER_HZ 1000

typedef void (*timer_tick_fn)(void);

void timer_early_init(void);    /* first thing in kmain: clock calibration */
void timer_init(void);
/* Start the local timer of an application processor. */
void timer_init_cpu(void);
/* Monotonic milliseconds since timer_early_init, read from the clock of the
 * architecture. */
uint64_t timer_ms(void);
/* The rate of the clock that timer_ms counts (the TSC on x86_64, the
 * generic timer on aarch64), in Hz. */
uint64_t timer_clock_hz(void);
/* The same time in nanoseconds. */
uint64_t timer_ns(void);
/* The same time in scheduler ticks (TIMER_HZ per second). */
uint64_t timer_ticks(void);
/* Number of timer interrupts taken by the boot CPU. */
uint64_t timer_interrupts(void);
/* Delay the caller by at least ms. Busy waits, or blocks once the scheduler
 * is running. A wake before the deadline starts another sleep. */
void sleep_ms(uint64_t ms);
/* sleep_ms_interruptible sleeps like sleep_ms, but ends when interrupted
 * returns true, such as signal_should_interrupt for a system call.  The
 * result is 0, or -EINTR after an interruption. */
int sleep_ms_interruptible(uint64_t ms, bool (*interrupted)(void));
/* Called from the timer interrupt on every tick, used by the scheduler. */
void timer_set_tick_handler(timer_tick_fn fn);
