#include <tests/ktest.h>
#include <sched/thread.h>
#include <sched/sched.h>
#include <sync/mutex.h>
#include <sync/semaphore.h>
#include <sync/condvar.h>
#include <drivers/timer.h>
#include <console.h>

/* M7: threads at several priorities, mutual exclusion, producer/consumer
 * through a semaphore and a condition variable, sleeping and joining. */

static struct mutex counter_lock;
static volatile long counter;

struct worker_arg {
    int id;
    int level;
    int iterations;
    int seen_level;
};

static void worker(void *p)
{
    struct worker_arg *a = p;
    for (int i = 0; i < a->iterations; i++) {
        mutex_lock(&counter_lock);
        long v = counter;
        sched_yield();          /* invite contention while the mutex is locked */
        counter = v + 1;
        mutex_unlock(&counter_lock);
        if ((i % 200) == 0)
            kprintf("worker %d level %d iteration %d\n", a->id, thread_current()->level, i);
    }
    a->seen_level = thread_current()->level;
    thread_exit(a->id * 10);
}

/* CPU bound thread that yields only at preemption points. */
static void spinner(void *p)
{
    volatile uint64_t *stop = p;
    uint64_t n = 0;
    while (!*stop) {
        n++;
        sched_preempt();
    }
    kprintf("spinner ran %lu loops, finished at level %d\n", n, thread_current()->level);
    thread_exit((int)thread_current()->level);
}

#define QSIZE 4
static struct {
    struct mutex lock;
    struct condvar not_full, not_empty;
    int items[QSIZE];
    int head, tail, count;
} queue;
static struct semaphore produced;

static void producer(void *p)
{
    for (int i = 1; i <= 50; i++) {
        mutex_lock(&queue.lock);
        while (queue.count == QSIZE)
            condvar_wait(&queue.not_full, &queue.lock);
        queue.items[queue.tail] = i;
        queue.tail = (queue.tail + 1) % QSIZE;
        queue.count++;
        condvar_signal(&queue.not_empty);
        mutex_unlock(&queue.lock);
        semaphore_up(&produced);
        if (i % 10 == 0)
            sleep_ms(2);
    }
}

static void consumer(void *p)
{
    long *sum = p;
    for (int i = 0; i < 50; i++) {
        semaphore_down(&produced);
        mutex_lock(&queue.lock);
        while (queue.count == 0)
            condvar_wait(&queue.not_empty, &queue.lock);
        *sum += queue.items[queue.head];
        queue.head = (queue.head + 1) % QSIZE;
        queue.count--;
        condvar_signal(&queue.not_full);
        mutex_unlock(&queue.lock);
    }
}

static void sleeper(void *p)
{
    uint64_t *slept = p;
    uint64_t t0 = timer_ticks();
    sleep_ms(30);
    *slept = timer_ticks() - t0;
}

static void test_sched(void)
{
    mutex_init(&counter_lock, "counter");
    struct worker_arg args[4];
    struct thread *workers[4];
    for (int i = 0; i < 4; i++) {
        args[i].id = i;
        args[i].level = i * 2;
        args[i].iterations = 500;
        args[i].seen_level = -1;
        workers[i] = thread_create("worker", worker, &args[i], args[i].level);
        ktest_assert(workers[i] != NULL, "thread_create failed");
    }
    for (int i = 0; i < 4; i++) {
        int code = thread_join(workers[i]);
        ktest_assert(code == i * 10, "worker %d exit code %d", i, code);
    }
    ktest_assert(counter == 2000, "counter %ld, expected 2000", counter);

    /* A CPU bound thread is demoted by slice exhaustion; a sleeping thread
     * maintains a good level. */
    volatile uint64_t stop = 0;
    struct thread *sp = thread_create("spinner", spinner, (void *)&stop, 0);
    uint64_t slept = 0;
    struct thread *sl = thread_create("sleeper", sleeper, &slept, 0);
    sleep_ms(120);
    stop = 1;
    int final_level = thread_join(sp);
    thread_join(sl);
    ktest_assert(final_level > 0, "spinner was never demoted");
    ktest_assert(slept >= 30 && slept < 300, "sleeper slept %lu ticks", slept);

    /* Producer and consumer. */
    mutex_init(&queue.lock, "queue");
    condvar_init(&queue.not_full, "not_full");
    condvar_init(&queue.not_empty, "not_empty");
    semaphore_init(&produced, 0, "produced");
    long sum = 0;
    struct thread *c = thread_create("consumer", consumer, &sum, 3);
    struct thread *pr = thread_create("producer", producer, NULL, 0);
    thread_join(pr);
    thread_join(c);
    ktest_assert(sum == 50 * 51 / 2, "consumer sum %ld", sum);

    /* Boost: after more than a second every runnable thread is back at 0. */
    sleep_ms(1100);
    ktest_assert(thread_current()->level == 0, "kinit at level %d after boost", thread_current()->level);
    sched_dump();
    kprintf("sched: uptime %lu ms\n", timer_ms());
}
KTEST_DEFINE("sched", test_sched);
