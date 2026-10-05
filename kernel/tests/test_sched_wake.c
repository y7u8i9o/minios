/* A stale wake of the scheduler (docs/postmortems/2026-10-05-sleep-wakeup.md).
 * interrupt_threads wakes a thread through its wait queue and then through
 * sched_wake.  The second call can read the state BLOCKED before the
 * first wake is consumed and queue a wake after it.  That wake can reach
 * the thread later, while it sleeps in sleep_ms, possibly on the sleeper
 * list of another CPU.
 *
 * The victim thread blocks on a wait queue and then sleeps 1 ms, VICTIM_ROUNDS
 * times.  The waker wakes it the way interrupt_threads does: waitq_wake_one
 * and then sched_wake at once.  Two more threads call sched_wake on the
 * victim without a pause, so that stale wakes are frequent.  The test
 * requires that every round ends and that every sleep of the victim lasts
 * at least its 1 ms.  The kernel must not panic and must not corrupt a
 * sleeper list. */
#include <tests/ktest.h>
#include <sched/thread.h>
#include <sched/sched.h>
#include <sched/wait.h>
#include <sync/spinlock.h>
#include <drivers/timer.h>
#include <console.h>

#define VICTIM_ROUNDS 3000
#define ATTACKERS 2

static DEFINE_SPINLOCK(race_lock);
static DEFINE_WAITQ(race_waitq);
static bool race_ready;             /* race_lock: the waker has woken the victim */
static bool race_done;              /* atomic: the victim finished */
static unsigned short_sleeps;       /* written by the victim only */
static struct thread *victim;

static void victim_fn(void *arg)
{
    for (int i = 0; i < VICTIM_ROUNDS; i++) {
        spin_lock(&race_lock);
        while (!race_ready)
            waitq_wait(&race_waitq, &race_lock);
        race_ready = false;
        spin_unlock(&race_lock);
        uint64_t start = timer_ns();
        sleep_ms(1);
        if (timer_ns() - start < 1000000)
            short_sleeps++;
    }
    __atomic_store_n(&race_done, true, __ATOMIC_RELEASE);
}

static void waker_fn(void *arg)
{
    while (!__atomic_load_n(&race_done, __ATOMIC_ACQUIRE)) {
        spin_lock(&race_lock);
        race_ready = true;
        waitq_wake_one(&race_waitq);
        spin_unlock(&race_lock);
        /* interrupt_threads: the wait queue wake, then sched_wake. */
        sched_wake(victim);
        sched_preempt();
    }
}

static void attacker_fn(void *arg)
{
    while (!__atomic_load_n(&race_done, __ATOMIC_ACQUIRE)) {
        sched_wake(victim);
        sched_preempt();
    }
}

static void test_sched_wake_race(void)
{
    victim = thread_create("victim", victim_fn, NULL, 0);
    ktest_assert(victim, "cannot create the victim");
    struct thread *waker = thread_create("waker", waker_fn, NULL, 0);
    ktest_assert(waker, "cannot create the waker");
    struct thread *attackers[ATTACKERS];
    for (int i = 0; i < ATTACKERS; i++) {
        attackers[i] = thread_create("attacker", attacker_fn, NULL, 0);
        ktest_assert(attackers[i], "cannot create attacker %d", i);
    }
    ktest_assert(thread_join(victim) == 0, "victim status");
    thread_join(waker);
    for (int i = 0; i < ATTACKERS; i++)
        thread_join(attackers[i]);
    kprintf("sched_wake_race: %d rounds, %u sleeps shorter than 1 ms\n", VICTIM_ROUNDS, short_sleeps);
    ktest_assert(short_sleeps == 0, "%u sleeps ended before their deadline", short_sleeps);
    kprintf("sched_wake_race: ok\n");
}
KTEST_DEFINE("sched_wake_race", test_sched_wake_race);
