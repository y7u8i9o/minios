/* The network clock: timer_ms, or a value under test control. The two
 * words are written by the controlling test and read by the worker and
 * the protocol code with acquire loads; controlled is read first so a
 * reader never mixes a stale value with the mode. */
#include <net/clock.h>
#include <drivers/timer.h>

static bool controlled;
static uint64_t controlled_ms;

uint64_t net_clock_ms(void)
{
    if (__atomic_load_n(&controlled, __ATOMIC_ACQUIRE))
        return __atomic_load_n(&controlled_ms, __ATOMIC_ACQUIRE);
    return timer_ms();
}

bool net_clock_is_controlled(void)
{
    return __atomic_load_n(&controlled, __ATOMIC_ACQUIRE);
}

void net_clock_control(bool on)
{
    if (on) {
        __atomic_store_n(&controlled_ms, timer_ms(), __ATOMIC_RELEASE);
        __atomic_store_n(&controlled, true, __ATOMIC_RELEASE);
    } else {
        __atomic_store_n(&controlled, false, __ATOMIC_RELEASE);
    }
}

void net_clock_set(uint64_t ms)
{
    if (__atomic_load_n(&controlled, __ATOMIC_ACQUIRE))
        __atomic_store_n(&controlled_ms, ms, __ATOMIC_RELEASE);
}

void net_clock_advance(uint64_t ms)
{
    if (__atomic_load_n(&controlled, __ATOMIC_ACQUIRE))
        __atomic_fetch_add(&controlled_ms, ms, __ATOMIC_ACQ_REL);
}
