#include <tests/ktest.h>
#include <arch/smp.h>
#include <arch/cpu.h>
#include <sched/thread.h>
#include <sched/sched.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <mm/tlb.h>
#include <lib/cmdline.h>
#include <drivers/timer.h>
#include <console.h>

/* M18: the configured number of CPUs is running, kernel threads execute
 * on several CPUs at the same time, and a kernel unmap reaches every CPU
 * through the shootdown IPI. */

#define NWORKERS 8
#define SPIN_MS  300

static void worker(void *arg);

static volatile int max_running;  /* most CPUs seen executing a worker at once */
static struct {
    cpu_mask_t cpus;
    unsigned long loops;
} results[NWORKERS];

/* Count the CPUs whose running thread is a worker right now. The reads
 * race with switches on other CPUs, which only makes the sample low. */
static void sample_concurrency(void)
{
    int n = 0;
    for (unsigned i = 0; i < smp_cpu_count(); i++) {
        struct thread *t = cpu_by_id(i)->current;
        if (t && t->entry == worker)
            n++;
    }
    int m = max_running;
    while (n > m && !__atomic_compare_exchange_n(&max_running, &m, n, false,
                                                 __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        ;
}

static void worker(void *arg)
{
    int id = (int)(long)arg;
    uint64_t until = timer_ticks() + SPIN_MS;
    while (timer_ticks() < until) {
        __atomic_fetch_or(&results[id].cpus, 1UL << cpu_current()->id, __ATOMIC_RELAXED);
        results[id].loops++;
        if ((results[id].loops & 0xff) == 0)
            sample_concurrency();
        sched_preempt();
    }
}

static void test_smp(void)
{
    char val[16];
    unsigned expected = 1;
    if (cmdline_lookup("cpus", val, sizeof val) && val[0] >= '1' && val[0] <= '9')
        expected = (unsigned)(val[0] - '0');
    kprintf("smp: %u cpus, expecting %u\n", smp_cpu_count(), expected);
    ktest_assert(smp_cpu_count() == expected, "%u cpus, expected %u", smp_cpu_count(), expected);
    ktest_assert(smp_online_mask() == (1UL << expected) - 1, "online mask %lx", smp_online_mask());

    struct thread *threads[NWORKERS];
    for (int i = 0; i < NWORKERS; i++) {
        threads[i] = thread_create("worker", worker, (void *)(long)i, 0);
        ktest_assert(threads[i] != NULL, "cannot create worker %d", i);
    }
    cpu_mask_t all = 0;
    for (int i = 0; i < NWORKERS; i++) {
        thread_join(threads[i]);
        kprintf("worker %d: cpus %lx, %lu loops\n", i, results[i].cpus, results[i].loops);
        all |= results[i].cpus;
    }
    kprintf("peak concurrency %d, cpus used %lx\n", max_running, all);
    ktest_assert(max_running >= 2, "workers never ran concurrently");
    ktest_assert(all == (1UL << expected) - 1, "not every cpu ran a worker: %lx", all);

    /* A kernel range flush must reach every other CPU. */
    struct tlb_stats before, after;
    tlb_get_stats(&before);
    void *stack = kstack_alloc();
    ktest_assert(stack != NULL, "kstack_alloc");
    kstack_free(stack);
    tlb_get_stats(&after);
    kprintf("shootdown: %lu requests, %lu ipis, %lu acks\n", after.requests, after.ipis, after.acks);
    ktest_assert(after.requests > before.requests, "no shootdown sent for a kernel unmap");
    ktest_assert(after.ipis - before.ipis >= expected - 1, "unmap reached %lu cpus",
                 after.ipis - before.ipis);
    ktest_assert(after.acks == after.ipis, "%lu ipis but %lu acks", after.ipis, after.acks);
}
KTEST_DEFINE("smp", test_smp);
