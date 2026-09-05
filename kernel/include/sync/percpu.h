#pragma once
#include <kernel.h>
#include <arch/cpu.h>
#include <arch/smp.h>

/* A cache line per CPU prevents unrelated counters from sharing a line.
 * Updates are relaxed because the sum is statistics, not synchronization. */
struct percpu_counter_slot {
    int64_t value;
    uint8_t pad[64 - sizeof(int64_t)];
} __aligned(64);

struct percpu_counter {
    struct percpu_counter_slot cpu[MAX_CPUS];
};

static inline void percpu_counter_init(struct percpu_counter *c, int64_t value)
{
    for (unsigned i = 0; i < MAX_CPUS; i++)
        __atomic_store_n(&c->cpu[i].value, i == 0 ? value : 0, __ATOMIC_RELAXED);
}

static inline void percpu_counter_add(struct percpu_counter *c, int64_t delta)
{
    __atomic_fetch_add(&c->cpu[cpu_current()->id].value, delta, __ATOMIC_RELAXED);
}

static inline void percpu_counter_inc(struct percpu_counter *c)
{
    percpu_counter_add(c, 1);
}

static inline void percpu_counter_dec(struct percpu_counter *c)
{
    percpu_counter_add(c, -1);
}

static inline int64_t percpu_counter_sum(const struct percpu_counter *c)
{
    int64_t sum = 0;
    unsigned n = smp_cpu_count();
    for (unsigned i = 0; i < n; i++)
        sum += __atomic_load_n(&c->cpu[i].value, __ATOMIC_RELAXED);
    return sum;
}
