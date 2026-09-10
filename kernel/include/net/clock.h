#pragma once
/* The clock of the network stack (N00): monotonic milliseconds, read from
 * timer_ms unless a test has taken control of it. Every protocol deadline
 * is computed and compared against this clock, never against timer_ms
 * directly, so the deterministic tests can move time without waiting.
 * Control is meant for kernel self tests only; releasing it returns the
 * clock to real time, which may lie before the last controlled value. */
#include <kernel.h>

uint64_t net_clock_ms(void);
/* True while a test controls the clock. */
bool net_clock_is_controlled(void);
/* Take control, freezing the clock at its current value, or release it. */
void net_clock_control(bool on);
/* Set or advance the controlled clock; ignored while it is not controlled. */
void net_clock_set(uint64_t ms);
void net_clock_advance(uint64_t ms);
