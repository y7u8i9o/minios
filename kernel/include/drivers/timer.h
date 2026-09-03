#pragma once
#include <kernel.h>

#define TIMER_HZ 1000

typedef void (*timer_tick_fn)(void);

void timer_init(void);
/* Start the local timer of an application processor. */
void timer_init_cpu(void);
/* Monotonic milliseconds since timer_init, read from the TSC. */
uint64_t timer_ms(void);
/* The same time in nanoseconds. */
uint64_t timer_ns(void);
/* The same time in scheduler ticks (TIMER_HZ per second). */
uint64_t timer_ticks(void);
/* Number of timer interrupts taken by the boot CPU. */
uint64_t timer_interrupts(void);
/* Delay the caller. Busy waits, or blocks once the scheduler is running. */
void sleep_ms(uint64_t ms);
/* Called from the timer interrupt on every tick, used by the scheduler. */
void timer_set_tick_handler(timer_tick_fn fn);
