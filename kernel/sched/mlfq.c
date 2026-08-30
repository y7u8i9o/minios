#define KLOG_SUBSYS "sched"
#include <sched/sched.h>
#include <sched/thread.h>
#include <arch/fpu.h>
#include <sched/proc.h>
#include <arch/cpu.h>
#include <arch/smp.h>
#include <arch/gdt.h>
#include <arch/trap.h>
#include <mm/vmm.h>
#include <drivers/timer.h>
#include <lib/string.h>
#include <kassert.h>
#include <klog.h>
#include <console.h>
#include <debug/panic.h>

/*
 * Multilevel feedback queue. Level 0 has the highest priority and a 10 ms
 * slice; each lower level doubles the slice. A thread that exhausts its
 * slice is demoted, a thread that blocks before that is promoted, and
 * every second all ready threads return to level 0.
 *
 * Every CPU has its own set of run queues. A woken thread goes to an idle
 * CPU when there is one, otherwise back to the CPU that last ran it. A CPU
 * whose queues are empty steals the best ready thread from another CPU.
 * One lock, sched_lock, covers every queue: switches are short and the
 * lock is already held across them, so per queue locks would add ordering
 * rules without removing contention that matters on a few CPUs.
 */
#define BASE_SLICE_MS   10
#define BOOST_INTERVAL  1000

struct spinlock sched_lock = SPINLOCK_INIT("sched_lock");

/* Everything below is protected by sched_lock. */
struct run_queues {
    struct list_head levels[MLFQ_LEVELS];
    unsigned nr_ready;
};
static struct run_queues rq[MAX_CPUS];
static struct list_head sleepers;          /* sorted by wake_at */
static bool started;
static uint64_t last_boost;

void context_switch(uint64_t **old_sp, uint64_t *new_sp);
extern char boot_stack_top[];

static inline int slice_for(int level)
{
    return BASE_SLICE_MS << level;
}

static inline bool cpu_is_idle(struct cpu *c)
{
    return c->started && c->current == c->idle && rq[c->id].nr_ready == 0;
}

/* Choose the CPU whose queue receives a thread that became ready. */
static unsigned pick_cpu(struct thread *t)
{
    unsigned n = smp_cpu_count();
    if (n == 1)
        return 0;
    unsigned home = t->cpu < n ? t->cpu : 0;
    if (cpu_is_idle(cpu_by_id(home)))
        return home;
    for (unsigned i = 0; i < n; i++) {
        if (cpu_is_idle(cpu_by_id(i)))
            return i;
    }
    return home;
}

static void enqueue(struct thread *t)
{
    kassert(spin_holding(&sched_lock));
    unsigned cpu = pick_cpu(t);
    t->state = THREAD_READY;
    t->cpu = cpu;
    list_add_tail(&t->run_link, &rq[cpu].levels[t->level]);
    rq[cpu].nr_ready++;
}

static struct thread *dequeue_best(unsigned cpu)
{
    for (int l = 0; l < MLFQ_LEVELS; l++) {
        if (!list_empty(&rq[cpu].levels[l])) {
            struct thread *t = list_first_entry(&rq[cpu].levels[l], struct thread, run_link);
            list_del(&t->run_link);
            rq[cpu].nr_ready--;
            return t;
        }
    }
    return NULL;
}

/* Take the best thread from the own queues, else steal the best thread of
 * any other CPU, else idle. */
static struct thread *sched_pick_next(struct cpu *c)
{
    struct thread *t = dequeue_best(c->id);
    if (t)
        return t;
    unsigned n = smp_cpu_count();
    int best_level = MLFQ_LEVELS;
    unsigned best_cpu = 0;
    for (unsigned i = 0; i < n; i++) {
        if (i == c->id || rq[i].nr_ready == 0)
            continue;
        for (int l = 0; l < best_level; l++) {
            if (!list_empty(&rq[i].levels[l])) {
                best_level = l;
                best_cpu = i;
                break;
            }
        }
    }
    if (best_level < MLFQ_LEVELS)
        return dequeue_best(best_cpu);
    return c->idle;
}

bool sched_started(void)
{
    return started;
}

void sched_add(struct thread *t)
{
    spin_lock(&sched_lock);
    enqueue(t);
    spin_unlock(&sched_lock);
}

void sched_wake(struct thread *t)
{
    spin_lock(&sched_lock);
    if (t->state == THREAD_BLOCKED || t->state == THREAD_SLEEPING) {
        if (t->state == THREAD_SLEEPING)
            list_del(&t->run_link);
        /* Blocking before the slice ran out earns a promotion. */
        if (t->level > 0)
            t->level--;
        t->slice_left = slice_for(t->level);
        enqueue(t);
    }
    spin_unlock(&sched_lock);
}

/* Runs on the new thread's stack right after every switch. The zombie's
 * stack is no longer in use, so its joiners may free it. sched_lock is
 * dropped around the wakeup to respect the exit_lock -> waitq.lock ->
 * sched_lock ordering. */
void sched_finish_switch(void)
{
    struct cpu *c = cpu_current();
    struct thread *z = c->zombie_pending;
    if (!z)
        return;
    c->zombie_pending = NULL;
    spin_unlock(&sched_lock);
    spin_lock(&z->exit_lock);
    z->finished = true;
    spin_unlock(&z->exit_lock);
    waitq_wake_all(&z->exit_waitq);
    spin_lock(&sched_lock);
}

void sched_switch_locked(void)
{
    kassert(spin_holding(&sched_lock));
    struct cpu *c = cpu_current();
    struct thread *prev = c->current;
    kassert(prev->state != THREAD_RUNNING);
    kassert(c->cli_depth == 1);

    struct thread *next = sched_pick_next(c);
    if (prev->state == THREAD_ZOMBIE)
        c->zombie_pending = prev;
    c->need_resched = false;
    if (next == prev) {
        prev->state = THREAD_RUNNING;
        return;
    }
    next->state = THREAD_RUNNING;
    next->cpu = c->id;
    c->current = next;
    tss_set_rsp0((uintptr_t)next->kstack_top);
    c->kstack_top = next->kstack_top;
    if (next->proc->vm && next->proc->vm != c->vm)
        vmspace_activate(next->proc->vm);

    int intena = c->int_enabled;
    if (prev->fpu)
        fpu_save(prev->fpu);
    context_switch(&prev->ctx, next->ctx);
    c = cpu_current();
    if (c->current->fpu)
        fpu_restore(c->current->fpu);
    c->int_enabled = intena;
    sched_finish_switch();
}

void sched_yield(void)
{
    struct thread *t = thread_current();
    spin_lock(&sched_lock);
    struct cpu *c = cpu_current();
    if (t->slice_left <= 0) {
        if (t->level < MLFQ_LEVELS - 1)
            t->level++;
        t->slice_left = slice_for(t->level);
    }
    if (t != c->idle)
        enqueue(t);
    else
        t->state = THREAD_READY;
    sched_switch_locked();
    spin_unlock(&sched_lock);
}

bool sched_need_resched(void)
{
    return cpu_current()->need_resched;
}

void sched_preempt(void)
{
    if (cpu_current()->need_resched)
        sched_yield();
}

void sched_sleep_until(uint64_t tick)
{
    struct thread *t = thread_current();
    spin_lock(&sched_lock);
    t->wake_at = tick;
    t->state = THREAD_SLEEPING;
    struct list_head *pos;
    list_for_each(pos, &sleepers) {
        if (list_entry(pos, struct thread, run_link)->wake_at > tick)
            break;
    }
    list_add_tail(&t->run_link, pos);
    sched_switch_locked();
    spin_unlock(&sched_lock);
}

static void wake_sleepers_locked(uint64_t now)
{
    while (!list_empty(&sleepers)) {
        struct thread *t = list_first_entry(&sleepers, struct thread, run_link);
        if (t->wake_at > now)
            break;
        list_del(&t->run_link);
        if (t->level > 0)
            t->level--;
        t->slice_left = slice_for(t->level);
        enqueue(t);
    }
}

static void boost_locked(void)
{
    unsigned n = smp_cpu_count();
    for (unsigned i = 0; i < n; i++) {
        for (int l = 1; l < MLFQ_LEVELS; l++) {
            while (!list_empty(&rq[i].levels[l])) {
                struct thread *t = list_first_entry(&rq[i].levels[l], struct thread, run_link);
                list_del(&t->run_link);
                t->level = 0;
                t->slice_left = slice_for(0);
                list_add_tail(&t->run_link, &rq[i].levels[0]);
            }
        }
        struct cpu *c = cpu_by_id(i);
        struct thread *cur = c->current;
        if (c->started && cur && cur != c->idle) {
            cur->level = 0;
            cur->slice_left = slice_for(0);
        }
    }
}

static bool any_ready_locked(void)
{
    unsigned n = smp_cpu_count();
    for (unsigned i = 0; i < n; i++) {
        if (rq[i].nr_ready)
            return true;
    }
    return false;
}

/* Slice accounting for the calling CPU's running thread. */
static void tick_cpu_locked(struct cpu *c)
{
    struct thread *cur = c->current;
    if (cur != c->idle && --cur->slice_left <= 0)
        c->need_resched = true;
    if (cur == c->idle && !c->need_resched && any_ready_locked())
        c->need_resched = true;
}

void sched_tick(void)
{
    spin_lock(&sched_lock);
    uint64_t now = timer_ticks();
    wake_sleepers_locked(now);
    tick_cpu_locked(cpu_current());
    if (now - last_boost >= BOOST_INTERVAL) {
        last_boost = now;
        boost_locked();
    }
    spin_unlock(&sched_lock);
}

void sched_tick_cpu(void)
{
    spin_lock(&sched_lock);
    tick_cpu_locked(cpu_current());
    spin_unlock(&sched_lock);
}

/* Turn the calling CPU's current context into its idle thread. */
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
    spin_lock(&sched_lock);
    c->idle = t;
    c->current = t;
    c->kstack_top = stack_top;
    spin_unlock(&sched_lock);
    tss_set_rsp0((uintptr_t)stack_top);
}

void sched_init(void)
{
    for (unsigned i = 0; i < MAX_CPUS; i++) {
        for (int l = 0; l < MLFQ_LEVELS; l++)
            list_init(&rq[i].levels[l]);
        rq[i].nr_ready = 0;
    }
    list_init(&sleepers);

    /* The boot context becomes the idle thread of the boot CPU. */
    make_idle(cpu_current(), boot_stack_top);
    last_boost = timer_ticks();
    timer_set_tick_handler(sched_tick);
    started = true;
    klog_info("mlfq scheduler with %d levels, base slice %d ms, %u cpus",
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
        sti();
        hlt();
        sched_preempt();
    }
}

void sched_dump(void)
{
    spin_lock(&sched_lock);
    unsigned n = smp_cpu_count();
    for (unsigned i = 0; i < n; i++) {
        struct cpu *c = cpu_by_id(i);
        kprintf("cpu %u: running %s(%d), %u ready\n", i,
                c->current ? c->current->name : "-", c->current ? c->current->tid : 0,
                rq[i].nr_ready);
        for (int l = 0; l < MLFQ_LEVELS; l++) {
            if (list_empty(&rq[i].levels[l]))
                continue;
            struct list_head *pos;
            kprintf("  level %d:", l);
            list_for_each(pos, &rq[i].levels[l]) {
                struct thread *t = list_entry(pos, struct thread, run_link);
                kprintf(" %s(%d)", t->name, t->tid);
            }
            kprintf("\n");
        }
    }
    spin_unlock(&sched_lock);
}
