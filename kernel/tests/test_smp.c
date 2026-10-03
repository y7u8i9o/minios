#include <tests/ktest.h>
#include <arch/smp.h>
#include <arch/cpu.h>
#include <sched/thread.h>
#include <sched/sched.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <mm/tlb.h>
#include <mm/memlayout.h>
#include <arch/paging.h>
#include <lib/cmdline.h>
#include <drivers/timer.h>
#include <console.h>

/* M18: the configured number of CPUs is running, kernel threads execute
 * on several CPUs at the same time, and a kernel remap reaches the TLB of
 * every CPU: through the shootdown IPI on x86_64, through broadcast
 * invalidation on aarch64 (A8). */

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

/* The cross CPU remap check: the reader reads remap_va, which caches its
 * translation in the TLB of the reader's CPU, waits without touching the
 * page while the test unmaps and remaps it, then records what it reads. */
static uintptr_t remap_va;
static bool reader_ready, remapped;
static unsigned reader_cpu;
static uint64_t reader_value;

static void tlb_reader(void *arg)
{
    volatile uint64_t *probe = (volatile uint64_t *)remap_va;
    reader_cpu = cpu_current()->id;
    (void)*probe;
    __atomic_store_n(&reader_ready, true, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&remapped, __ATOMIC_ACQUIRE))
        cpu_relax();
    reader_value = *probe;
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
    kprintf("peak concurrency %d, cpus used %lx, ticks", max_running, all);
    for (unsigned i = 0; i < smp_cpu_count(); i++)
        kprintf(" %lu", __atomic_load_n(&cpu_by_id(i)->ticks, __ATOMIC_RELAXED));
    kprintf("\n");
    ktest_assert(max_running >= 2, "workers never ran concurrently");
    ktest_assert(all == (1UL << expected) - 1, "not every cpu ran a worker: %lx", all);

    /* A kernel range flush must reach every other CPU: a reader on another
     * CPU caches the translation of a kernel page, the page is remapped to
     * another frame, and the reader's next access must see that frame.
     * Kernel threads are not preempted, so the reader remains on its CPU
     * while it spins, and this thread cannot run on that CPU meanwhile. */
    struct tlb_stats before, after;
    void *stack = kstack_alloc();
    ktest_assert(stack != NULL, "kstack_alloc");
    remap_va = (uintptr_t)stack - PAGE_SIZE;
    uintptr_t old_pa;
    ktest_assert(vmm_translate(&kernel_vmspace, remap_va, &old_pa, NULL), "translate the stack page");
    *(volatile uint64_t *)remap_va = 0x1111;
    struct page *fresh = pmm_alloc_page();
    ktest_assert(fresh != NULL, "pmm_alloc_page");
    *(volatile uint64_t *)P2V(page_to_phys(fresh)) = 0x2222;
    struct thread *reader = thread_create("tlb_reader", tlb_reader, NULL, 0);
    ktest_assert(reader != NULL, "cannot create the reader");
    while (!__atomic_load_n(&reader_ready, __ATOMIC_ACQUIRE))
        sched_preempt();
    tlb_get_stats(&before);
    ktest_assert(vmm_unmap(&kernel_vmspace, remap_va, PAGE_SIZE) == 0, "unmap");
    ktest_assert(vmm_map(&kernel_vmspace, remap_va, page_to_phys(fresh), PAGE_SIZE,
                         VM_KERNEL_RW | VM_GLOBAL) == 0, "map");
    tlb_get_stats(&after);
    __atomic_store_n(&remapped, true, __ATOMIC_RELEASE);
    thread_join(reader);
    kprintf("remap seen by cpu %u from cpu %u: %lx\n", reader_cpu, cpu_current()->id, reader_value);
    ktest_assert(reader_value == 0x2222, "cpu %u read %lx after the remap", reader_cpu, reader_value);
    kprintf("shootdown: %lu requests, %lu ipis, %lu acks\n", after.requests, after.ipis, after.acks);
    if (!PAGING_TLB_BROADCAST) {
        ktest_assert(after.requests > before.requests, "no shootdown sent for a kernel unmap");
        ktest_assert(after.ipis - before.ipis >= expected - 1, "unmap reached %lu cpus",
                     after.ipis - before.ipis);
        ktest_assert(after.acks == after.ipis, "%lu ipis but %lu acks", after.ipis, after.acks);
    }
    /* vmm_unmap leaves the frames to the caller: the fresh one is freed
     * here, and the stack frame returns to the stack. */
    ktest_assert(vmm_unmap(&kernel_vmspace, remap_va, PAGE_SIZE) == 0, "unmap the fresh frame");
    pmm_free_page(fresh);
    ktest_assert(vmm_map(&kernel_vmspace, remap_va, old_pa, PAGE_SIZE, VM_KERNEL_RW | VM_GLOBAL) == 0,
                 "restore the stack frame");
    kstack_free(stack);
}
KTEST_DEFINE("smp", test_smp);
