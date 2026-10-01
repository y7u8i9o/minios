#include <tests/ktest.h>
#include <mm/vmm.h>
#include <mm/pmm.h>
#include <mm/memlayout.h>
#include <arch/smp.h>
#include <arch/cpu.h>
#include <arch/paging.h>
#include <sched/thread.h>
#include <sched/sched.h>
#include <console.h>

/* Address spaces with the same user address on several CPUs (A8 and the
 * ASID generations of aarch64). Every space maps its own frame at
 * ASID_VA. Threads on every CPU load the spaces in a pseudo random order
 * and read the address, which must always return the frame of the loaded
 * space. The case runs with asid_bits=3 on aarch64, so 24 spaces share
 * seven ASIDs and the generation advances many times. In the second phase
 * every space is remapped to a new frame while no thread runs, and the
 * reads must return the new frames: the flush by ASID must also reach
 * the TLBs of CPUs that loaded the space in an earlier generation. */

#define ASID_SPACES  24
#define ASID_THREADS 4
#define ASID_ROUNDS  400
#define ASID_VA      0x10000000UL

static struct vmspace *spaces[ASID_SPACES];
static uint64_t expected_base;
static int wrong_values;
static cpu_mask_t cpus_used;

static void asid_worker(void *arg)
{
    uint32_t seed = (uint32_t)(uintptr_t)arg * 2654435761u + 1;
    for (int r = 0; r < ASID_ROUNDS; r++) {
        seed = seed * 1103515245u + 12345u;
        unsigned i = (seed >> 16) % ASID_SPACES;
        /* Kernel threads are not preempted, so the space stays loaded on
         * this CPU between the activation and the read. */
        vmspace_activate(spaces[i]);
        uint64_t v = *(volatile uint64_t *)ASID_VA;
        if (v != expected_base + i)
            __atomic_fetch_add(&wrong_values, 1, __ATOMIC_RELAXED);
        __atomic_fetch_or(&cpus_used, 1UL << cpu_current()->id, __ATOMIC_RELAXED);
        if ((r & 7) == 7)
            sched_yield();
    }
    vmspace_activate(&kernel_vmspace);
}

static void run_phase(const char *name)
{
    struct thread *threads[ASID_THREADS];
    for (int t = 0; t < ASID_THREADS; t++) {
        threads[t] = thread_create("asid_worker", asid_worker, (void *)(uintptr_t)(t + 1), 0);
        ktest_assert(threads[t] != NULL, "cannot create a worker");
    }
    for (int t = 0; t < ASID_THREADS; t++)
        thread_join(threads[t]);
    kprintf("asid: %s, %d reads, %d wrong values\n", name, ASID_THREADS * ASID_ROUNDS, wrong_values);
    ktest_assert(wrong_values == 0, "%s: %d reads returned the frame of another space", name, wrong_values);
}

static void map_frames(uint64_t base, bool replace)
{
    for (int i = 0; i < ASID_SPACES; i++) {
        struct page *pg = pmm_alloc_page();
        ktest_assert(pg != NULL, "pmm_alloc_page");
        *(volatile uint64_t *)P2V(page_to_phys(pg)) = base + (uint64_t)i;
        /* The mapping holds the frame's reference, which the teardown of
         * the space drops (free_user_level). vmm_unmap leaves the
         * reference to the caller. */
        page_get(pg);
        if (replace) {
            uintptr_t old;
            ktest_assert(vmm_translate(spaces[i], ASID_VA, &old, NULL), "translate space %d", i);
            ktest_assert(vmm_unmap(spaces[i], ASID_VA, PAGE_SIZE) == 0, "unmap space %d", i);
            page_put(phys_to_page(old));
        }
        ktest_assert(vmm_map(spaces[i], ASID_VA, page_to_phys(pg), PAGE_SIZE,
                             VM_READ | VM_WRITE | VM_USER) == 0, "map space %d", i);
    }
}

static void test_asid(void)
{
    for (int i = 0; i < ASID_SPACES; i++) {
        spaces[i] = vmspace_create();
        ktest_assert(spaces[i] != NULL, "vmspace_create");
    }
    expected_base = 1000;
    map_frames(expected_base, false);
    run_phase("first frames");
    expected_base = 2000;
    map_frames(expected_base, true);
    run_phase("replaced frames");
    for (int i = 0; i < ASID_SPACES; i++) {
        vmspace_free_user_pages(spaces[i]);
        vmspace_destroy(spaces[i]);
    }
    kprintf("asid: %d spaces on cpus %lx, %lu rollovers\n", ASID_SPACES, cpus_used,
            paging_asid_rollovers());
    ktest_assert(smp_cpu_count() < 2 || (cpus_used & ~1UL), "the workers ran on one cpu only");
}
KTEST_DEFINE("asid", test_asid);
