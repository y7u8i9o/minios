#pragma once
#include <kernel.h>
#include <sync/mpsc.h>

/* Non-preemptible-kernel RCU.  Read sections may not sleep.  A callback is
 * eligible after every started CPU has reported a context-switch or idle
 * quiescent state at or beyond the callback's epoch. Callbacks run in a
 * dedicated kernel thread and may sleep; they never run in an interrupt. */
struct rcu_head {
    struct mpsc_node node;
    uint64_t epoch;
    void (*func)(struct rcu_head *head);
};

void rcu_read_lock(void);
void rcu_read_unlock(void);
bool rcu_read_held(void);
void rcu_quiescent(void);
void rcu_call(struct rcu_head *head, void (*func)(struct rcu_head *head));
/* Start the sole callback consumer after the scheduler is running. */
void rcu_start_worker(void);
