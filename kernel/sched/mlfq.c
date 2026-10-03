#define KLOG_SUBSYS "sched"
#include <debug/profile.h>
#include <sched/sched.h>
#include <sched/thread.h>
#include <arch/thread.h>
#include <sched/proc.h>
#include <arch/cpu.h>
#include <arch/smp.h>
#include <arch/irq.h>
#include <mm/vmm.h>
#include <drivers/timer.h>
#include <lib/string.h>
#include <kassert.h>
#include <klog.h>
#include <console.h>
#include <debug/panic.h>
#include <sync/rcu.h>

#define BASE_SLICE_MS   10
#define BOOST_INTERVAL  1000

/* Only the CPU of the run queue consumes inbound, and it changes levels
 * and sleepers under lock.
 * Remote producers publish only through the MPSC inbox. */
struct run_queues {
    struct spinlock lock;
    struct list_head levels[MLFQ_LEVELS];
    struct list_head sleepers;
    struct mpsc_head inbound;
    unsigned nr_ready;
    uint64_t last_boost;
} __aligned(64);

static struct run_queues rq[MAX_CPUS];
static bool started;

extern char boot_stack_top[];

static inline int slice_for(int level)
{
    return BASE_SLICE_MS << level;
}

static inline struct run_queues *local_rq(void)
{
    return &rq[cpu_current()->id];
}

void sched_lock_current(void)
{
    spin_lock(&local_rq()->lock);
}

void sched_unlock_current(void)
{
    spin_unlock(&local_rq()->lock);
}

static void enqueue_locked(struct run_queues *r, struct thread *t)
{
    kassert(spin_locked_by_current(&r->lock));
    if (t->waiting_on)
        panic("enqueue waiter %s tid %d state %d wq %p", t->name, t->tid, t->state, t->waiting_on);
    __atomic_store_n(&t->state, THREAD_READY, __ATOMIC_RELEASE);
    list_add_tail(&t->run_link, &r->levels[t->level]);
    r->nr_ready++;
}

static struct thread *dequeue_best_locked(struct run_queues *r)
{
    for (int l = 0; l < MLFQ_LEVELS; l++) {
        if (!list_empty(&r->levels[l])) {
            struct thread *t = list_first_entry(&r->levels[l], struct thread, run_link);
            list_del(&t->run_link);
            r->nr_ready--;
            return t;
        }
    }
    return NULL;
}

static void kick_cpu(unsigned id)
{
    struct cpu *c = cpu_by_id(id);
    __atomic_store_n(&c->need_resched, true, __ATOMIC_RELEASE);
    if (id != cpu_current()->id && c->started)
        arch_send_ipi(id, IRQ_RESCHED);
}

static bool queue_inbound(struct thread *t, unsigned target)
{
    bool expected = false;
    /* Sequentially consistent against the release of the claim in
     * drain_inbound_locked, see there. */
    if (!__atomic_compare_exchange_n(&t->wake_queued, &expected, true, false,
                                      __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        return false;
    __atomic_store_n(&t->cpu, target, __ATOMIC_RELEASE);
    mpsc_push(&rq[target].inbound, &t->wake_node);
    kick_cpu(target);
    return true;
}

static unsigned pick_cpu(void)
{
    unsigned n = smp_cpu_count();
    unsigned best = cpu_current()->id;
    unsigned best_load = ~0u;
    for (unsigned i = 0; i < n; i++) {
        struct cpu *c = cpu_by_id(i);
        if (!c->started && i != cpu_current()->id)
            continue;
        unsigned load = __atomic_load_n(&rq[i].nr_ready, __ATOMIC_RELAXED);
        struct thread *cur = __atomic_load_n(&c->current, __ATOMIC_ACQUIRE);
        if (cur && cur != c->idle)
            load++;
        if (load < best_load) {
            best = i;
            best_load = load;
        }
    }
    return best;
}

static void drain_inbound_locked(struct run_queues *r)
{
    struct mpsc_node *node = mpsc_reverse(mpsc_take_all(&r->inbound));
    while (node) {
        struct mpsc_node *next = node->next;
        struct thread *t = container_of(node, struct thread, wake_node);
        /* next was captured first, so the producer may reuse wake_node from
         * here on. Releasing the claim before reading the thread's state
         * matters: a wake that finds the claim taken pushes nothing, so
         * the state read below must observe every wake that observed the
         * claim. Both sides are sequentially consistent for that. */
        __atomic_store_n(&t->wake_queued, false, __ATOMIC_SEQ_CST);
        enum thread_state state = __atomic_load_n(&t->state, __ATOMIC_SEQ_CST);
        struct waitq *wq = __atomic_load_n(&t->waiting_on, __ATOMIC_SEQ_CST);
        /* A blocked thread still on a wait queue is not runnable. This wake
         * is stale, queued between an earlier wake and the thread blocking
         * again (signal delivery interrupts a wait and then wakes without
         * the queue lock acquired). The wake that removes it from the queue
         * queues it again. */
        if (state == THREAD_BLOCKED && wq) {
            node = next;
            continue;
        }
        if (state == THREAD_NEW || state == THREAD_BLOCKED ||
            state == THREAD_SLEEPING || state == THREAD_STOPPED) {
            if (state == THREAD_SLEEPING)
                list_del(&t->run_link);
            if (state != THREAD_NEW && t->level > 0)
                t->level--;
            t->slice_left = slice_for(t->level);
            enqueue_locked(r, t);
        }
        node = next;
    }
}

/* Victim locks are tried while the local lock is locked, never waited for. */
static struct thread *steal_best_locked(void)
{
    unsigned me = cpu_current()->id;
    unsigned n = smp_cpu_count();
    for (int level = 0; level < MLFQ_LEVELS; level++) {
        for (unsigned i = 0; i < n; i++) {
            if (i == me || __atomic_load_n(&rq[i].nr_ready, __ATOMIC_RELAXED) == 0)
                continue;
            struct run_queues *victim = &rq[i];
            if (!spin_try_lock(&victim->lock))
                continue;
            struct thread *t = NULL;
            if (!list_empty(&victim->levels[level])) {
                t = list_first_entry(&victim->levels[level], struct thread, run_link);
                list_del(&t->run_link);
                victim->nr_ready--;
                __atomic_store_n(&t->cpu, me, __ATOMIC_RELEASE);
            }
            spin_unlock(&victim->lock);
            if (t)
                return t;
        }
    }
    return NULL;
}

static struct thread *sched_pick_next_locked(struct run_queues *r)
{
    drain_inbound_locked(r);
    struct thread *t = dequeue_best_locked(r);
    if (!t)
        t = steal_best_locked();
    return t ? t : cpu_current()->idle;
}

bool sched_started(void)
{
    return __atomic_load_n(&started, __ATOMIC_ACQUIRE);
}

void sched_add(struct thread *t)
{
    kassert(__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) == THREAD_NEW);
    queue_inbound(t, pick_cpu());
}

void sched_wake(struct thread *t)
{
    /* Sequentially consistent against stop_current (ipc/signal.c). */
    enum thread_state state = __atomic_load_n(&t->state, __ATOMIC_SEQ_CST);
    if (state != THREAD_BLOCKED && state != THREAD_SLEEPING && state != THREAD_STOPPED)
        return;
    profile_ready(t);
    unsigned target = __atomic_load_n(&t->cpu, __ATOMIC_ACQUIRE);
    if (target >= smp_cpu_count())
        target = 0;
    queue_inbound(t, target);
}

void sched_finish_switch(void)
{
    struct cpu *c = cpu_current();
    struct thread *z = c->zombie_pending;
    if (!z)
        return;
    c->zombie_pending = NULL;
    sched_unlock_current();
    spin_lock(&z->exit_lock);
    z->finished = true;
    waitq_wake_all(&z->exit_waitq);
    /* A joiner may free z as soon as it observes finished. Retain the
     * condition lock until our last access to the embedded wait queue. */
    spin_unlock(&z->exit_lock);
    sched_lock_current();
}

void sched_switch_locked(void)
{
    struct run_queues *r = local_rq();
    kassert(spin_locked_by_current(&r->lock));
    struct cpu *c = cpu_current();
    struct thread *prev = c->current;
    kassert(__atomic_load_n(&prev->state, __ATOMIC_ACQUIRE) != THREAD_RUNNING);
    kassert(c->cli_depth == 1);
    rcu_quiescent();

    struct thread *next = sched_pick_next_locked(r);
    if (prev->state == THREAD_ZOMBIE)
        c->zombie_pending = prev;
    __atomic_store_n(&c->need_resched, false, __ATOMIC_RELEASE);
    if (next == prev) {
        __atomic_store_n(&prev->state, THREAD_RUNNING, __ATOMIC_RELEASE);
        return;
    }
    if (prev->state == THREAD_READY) {
        prev->nivcsw++;
        __atomic_fetch_add(&prev->proc->nivcsw, 1, __ATOMIC_RELAXED);
    } else {
        prev->nvcsw++;
        __atomic_fetch_add(&prev->proc->nvcsw, 1, __ATOMIC_RELAXED);
    }
    __atomic_store_n(&next->state, THREAD_RUNNING, __ATOMIC_RELEASE);
    __atomic_store_n(&next->cpu, c->id, __ATOMIC_RELEASE);
    c->current = next;
    arch_set_kernel_stack((uintptr_t)next->kstack_top);
    c->kstack_top = next->kstack_top;
    if (next->proc->vm && next->proc->vm != c->vm)
        vmspace_activate(next->proc->vm);

    int intena = c->int_enabled;
    profile_leave_cpu(prev, prev->state == THREAD_READY);
    arch_switch_to(prev, next);
    c = cpu_current();
    profile_enter_cpu(c->current);
    arch_thread_resume(c->current);
    c->int_enabled = intena;
    sched_finish_switch();
}

void sched_yield(void)
{
    struct thread *t = thread_current();
    sched_lock_current();
    struct run_queues *r = local_rq();
    struct cpu *c = cpu_current();
    if (t->slice_left <= 0) {
        if (t->level < MLFQ_LEVELS - 1)
            t->level++;
        t->slice_left = slice_for(t->level);
    }
    if (t != c->idle)
        enqueue_locked(r, t);
    else
        __atomic_store_n(&t->state, THREAD_READY, __ATOMIC_RELEASE);
    sched_switch_locked();
    sched_unlock_current();
}

bool sched_need_resched(void)
{
    return __atomic_load_n(&cpu_current()->need_resched, __ATOMIC_ACQUIRE);
}

void sched_preempt(void)
{
    if (sched_need_resched())
        sched_yield();
}

void sched_sleep_until(uint64_t tick)
{
    struct thread *t = thread_current();
    sched_lock_current();
    struct run_queues *r = local_rq();
    t->wake_at = tick;
    __atomic_store_n(&t->state, THREAD_SLEEPING, __ATOMIC_RELEASE);
    struct list_head *pos;
    list_for_each(pos, &r->sleepers) {
        if (list_entry(pos, struct thread, run_link)->wake_at > tick)
            break;
    }
    list_add_tail(&t->run_link, pos);
    sched_switch_locked();
    sched_unlock_current();
}

static void wake_sleepers_locked(struct run_queues *r, uint64_t now)
{
    while (!list_empty(&r->sleepers)) {
        struct thread *t = list_first_entry(&r->sleepers, struct thread, run_link);
        if (t->wake_at > now)
            break;
        bool expected = false;
        if (!__atomic_compare_exchange_n(&t->wake_queued, &expected, true, false,
                                          __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            break;
        list_del(&t->run_link);
        if (t->level > 0)
            t->level--;
        t->slice_left = slice_for(t->level);
        enqueue_locked(r, t);
        __atomic_store_n(&t->wake_queued, false, __ATOMIC_RELEASE);
    }
}

static void boost_locked(struct run_queues *r)
{
    for (int l = 1; l < MLFQ_LEVELS; l++) {
        while (!list_empty(&r->levels[l])) {
            struct thread *t = list_first_entry(&r->levels[l], struct thread, run_link);
            list_del(&t->run_link);
            t->level = 0;
            t->slice_left = slice_for(0);
            list_add_tail(&t->run_link, &r->levels[0]);
        }
    }
    struct cpu *c = cpu_current();
    struct thread *cur = c->current;
    if (cur && cur != c->idle) {
        cur->level = 0;
        cur->slice_left = slice_for(0);
    }
}

/* True if another CPU's run queue has a thread waiting, which an idle CPU
 * can steal. The counts are read without the locks. A stale count causes
 * one extra rescheduling of the idle thread or delays a steal by a tick. */
static bool others_have_ready(unsigned me)
{
    unsigned n = smp_cpu_count();
    for (unsigned i = 0; i < n; i++)
        if (i != me && __atomic_load_n(&rq[i].nr_ready, __ATOMIC_RELAXED))
            return true;
    return false;
}

static void tick_local(void)
{
    struct cpu *c = cpu_current();
    struct run_queues *r = local_rq();
    /* Only this CPU runs and accounts current, so the common tick path
     * changes its slice without touching the run-queue lock. */
    struct thread *cur = c->current;
    if (cur != c->idle && --cur->slice_left <= 0)
        __atomic_store_n(&c->need_resched, true, __ATOMIC_RELEASE);
    spin_lock(&r->lock);
    drain_inbound_locked(r);
    uint64_t now = timer_ticks();
    wake_sleepers_locked(r, now);
    if (cur == c->idle && (r->nr_ready || others_have_ready(c->id)))
        __atomic_store_n(&c->need_resched, true, __ATOMIC_RELEASE);
    if (now - r->last_boost >= BOOST_INTERVAL) {
        r->last_boost = now;
        boost_locked(r);
    }
    spin_unlock(&r->lock);
}

void sched_tick(void)
{
    tick_local();
}

void sched_tick_cpu(void)
{
    tick_local();
}

static void resched_irq(struct trapframe *tf, void *arg)
{
    __atomic_store_n(&cpu_current()->need_resched, true, __ATOMIC_RELEASE);
}

static void make_idle(struct cpu *c, void *stack_top)
{
    struct thread *t = thread_alloc(&kernel_proc, "idle", NULL, NULL, MLFQ_LEVELS - 1);
    if (!t)
        panic("sched: cannot allocate idle thread for cpu %u", c->id);
    kstack_free(t->kstack_top);
    t->kstack_top = stack_top;
    t->on_boot_stack = true;
    t->state = THREAD_RUNNING;
    t->cpu = c->id;
    spin_lock(&rq[c->id].lock);
    c->idle = t;
    c->current = t;
    c->kstack_top = stack_top;
    spin_unlock(&rq[c->id].lock);
    arch_set_kernel_stack((uintptr_t)stack_top);
}

void sched_init(void)
{
    uint64_t now = timer_ticks();
    for (unsigned i = 0; i < MAX_CPUS; i++) {
        spinlock_init(&rq[i].lock, "run_queue");
        for (int l = 0; l < MLFQ_LEVELS; l++)
            list_init(&rq[i].levels[l]);
        list_init(&rq[i].sleepers);
        mpsc_init(&rq[i].inbound);
        rq[i].nr_ready = 0;
        rq[i].last_boost = now;
    }
    irq_register(IRQ_RESCHED, resched_irq, NULL);
    make_idle(cpu_current(), boot_stack_top);
    timer_set_tick_handler(sched_tick);
    __atomic_store_n(&started, true, __ATOMIC_RELEASE);
    klog_info("per-cpu mlfq with %d levels, base slice %d ms, %u cpus",
              MLFQ_LEVELS, BASE_SLICE_MS, smp_cpu_count());
}

void sched_init_cpu(void)
{
    struct cpu *c = cpu_current();
    make_idle(c, c->ap_stack_top);
}

__noreturn void sched_idle_loop(void)
{
    for (;;) {
        rcu_quiescent();
        arch_idle();
        sched_preempt();
    }
}

void sched_dump(void)
{
    unsigned n = smp_cpu_count();
    for (unsigned i = 0; i < n; i++) {
        struct run_queues *r = &rq[i];
        spin_lock(&r->lock);
        struct cpu *c = cpu_by_id(i);
        kprintf("cpu %u: running %s(%d), %u ready\n", i,
                c->current ? c->current->name : "-", c->current ? c->current->tid : 0,
                r->nr_ready);
        for (int l = 0; l < MLFQ_LEVELS; l++) {
            if (list_empty(&r->levels[l]))
                continue;
            struct list_head *pos;
            kprintf("  level %d:", l);
            list_for_each(pos, &r->levels[l]) {
                struct thread *t = list_entry(pos, struct thread, run_link);
                kprintf(" %s(%d)", t->name, t->tid);
            }
            kprintf("\n");
        }
        spin_unlock(&r->lock);
    }
}
