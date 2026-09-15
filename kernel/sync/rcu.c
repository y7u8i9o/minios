#include <sync/rcu.h>
#include <arch/cpu.h>
#include <arch/smp.h>
#include <debug/panic.h>
#include <sched/thread.h>
#include <drivers/timer.h>

static uint64_t rcu_epoch = 1;

void rcu_read_lock(void)
{
    struct cpu *c = cpu_current();
    c->rcu_read_depth++;
    __atomic_signal_fence(__ATOMIC_ACQUIRE);
}

void rcu_read_unlock(void)
{
    struct cpu *c = cpu_current();
    if (c->rcu_read_depth == 0)
        panic("rcu_read_unlock without rcu_read_lock");
    __atomic_signal_fence(__ATOMIC_RELEASE);
    c->rcu_read_depth--;
}

bool rcu_read_held(void)
{
    return cpu_current()->rcu_read_depth != 0;
}

void rcu_quiescent(void)
{
    struct cpu *c = cpu_current();
    if (c->rcu_read_depth == 0) {
        uint64_t epoch = __atomic_load_n(&rcu_epoch, __ATOMIC_ACQUIRE);
        __atomic_store_n(&c->rcu_epoch, epoch, __ATOMIC_RELEASE);
    }
}

void rcu_call(struct rcu_head *head, void (*func)(struct rcu_head *head))
{
    head->func = func;
    head->epoch = __atomic_add_fetch(&rcu_epoch, 1, __ATOMIC_ACQ_REL);
    mpsc_push(&cpu_current()->rcu_callbacks, &head->node);
}

static bool grace_period_done(uint64_t epoch)
{
    unsigned n = smp_cpu_count();
    for (unsigned i = 0; i < n; i++) {
        struct cpu *c = cpu_by_id(i);
        if (__atomic_load_n(&c->started, __ATOMIC_ACQUIRE) &&
            __atomic_load_n(&c->rcu_epoch, __ATOMIC_ACQUIRE) < epoch)
            return false;
    }
    return true;
}

static void reclaim_cpu(struct cpu *c)
{
    struct mpsc_head *q = &c->rcu_callbacks;
    struct mpsc_node *list = mpsc_reverse(mpsc_take_all(q));
    while (list) {
        struct mpsc_node *next = list->next;
        struct rcu_head *head = container_of(list, struct rcu_head, node);
        if (grace_period_done(head->epoch))
            head->func(head);
        else
            mpsc_push(q, list);
        list = next;
    }
}

static void rcu_worker(void *arg)
{
    for (;;) {
        for (unsigned i = 0; i < smp_cpu_count(); i++)
            reclaim_cpu(cpu_by_id(i));
        sleep_ms(1);
    }
}

struct sync_head {
    struct rcu_head head;
    volatile bool done;
};

static void sync_done(struct rcu_head *head)
{
    struct sync_head *sh = container_of(head, struct sync_head, head);
    __atomic_store_n(&sh->done, true, __ATOMIC_RELEASE);
}

void rcu_synchronize(void)
{
    struct sync_head sh = { .done = false };
    rcu_call(&sh.head, sync_done);
    while (!__atomic_load_n(&sh.done, __ATOMIC_ACQUIRE))
        sleep_ms(1);
}

void rcu_start_worker(void)
{
    if (!thread_create("rcu", rcu_worker, NULL, 0))
        panic("cannot create RCU worker");
}
