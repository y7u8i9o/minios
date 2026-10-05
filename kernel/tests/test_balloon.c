/* V4 of docs/plan/release-0.6.0.md: the memory balloon. The QMP scripts of
 * tests/cases/balloon and tests/cases/balloon_oom set the target with the
 * command balloon after the line "ready" of each test. */
#include <tests/ktest.h>
#include <console.h>
#include <drivers/virtio/virtio_balloon.h>
#include <drivers/timer.h>
#include <fs/vfs.h>
#include <lib/printf.h>
#include <lib/string.h>
#include <mm/memlayout.h>
#include <mm/pmm.h>
#include <mm/slab.h>
#include <mm/swap.h>
#include <sched/proc.h>
#include <sched/user.h>
#include <errno.h>

/* Waits up to 60 s until the balloon has the size of its target. The
 * target is zero when target_zero is set and nonzero otherwise. */
static void wait_balloon(struct balloon_info *info, bool target_zero)
{
    for (int i = 0; i < 600; i++) {
        virtio_balloon_get_info(info);
        if ((info->target == 0) == target_zero && info->size == info->target)
            return;
        sleep_ms(100);
    }
    ktest_assert(false, "balloon of %lu pages, target %lu", (unsigned long)info->size,
                 (unsigned long)info->target);
}

/* The value of a line "Key: N kB" of a file in /dev. */
static uint64_t read_kb(const char *path, const char *key)
{
    struct file *f;
    ktest_assert(vfs_open(path, O_RDONLY, 0, &f) == 0, "open %s", path);
    char text[512];
    long n = file_read(f, text, sizeof text - 1);
    file_put(f);
    ktest_assert(n > 0, "read %s: %ld", path, n);
    text[n] = '\0';
    size_t len = strlen(key);
    for (char *line = text; line && *line; line = strchr(line, '\n') ? strchr(line, '\n') + 1 : NULL)
        if (strncmp(line, key, len) == 0 && line[len] == ':')
            return strtoull(line + len + 1, NULL, 10);
    ktest_assert(false, "no %s in %s", key, path);
    return 0;
}

/* The host inflates the balloon to 128 MiB, asks for statistics and
 * deflates the balloon again. MemTotal follows the size. */
static void test_balloon(void)
{
    struct balloon_info info;
    virtio_balloon_get_info(&info);
    ktest_assert(info.present, "no balloon device");
    uint64_t total0 = read_kb("/dev/meminfo", "MemTotal");
    kprintf("balloon: ready, MemTotal %lu kB\n", (unsigned long)total0);
    wait_balloon(&info, false);
    uint64_t total = read_kb("/dev/meminfo", "MemTotal");
    uint64_t size_kb = read_kb("/dev/balloon", "BalloonSize");
    ktest_assert(size_kb == info.size * 4 && read_kb("/dev/balloon", "BalloonTarget") == info.target * 4,
                 "/dev/balloon reports %lu kB", (unsigned long)size_kb);
    ktest_assert(total == total0 - info.size * 4, "MemTotal %lu kB with a balloon of %lu pages",
                 (unsigned long)total, (unsigned long)info.size);
    kprintf("balloon: inflated to %lu pages, MemTotal %lu kB\n", (unsigned long)info.size, (unsigned long)total);
    for (int i = 0; i < 300 && info.stats_updates < 2; i++) {
        sleep_ms(100);
        virtio_balloon_get_info(&info);
    }
    ktest_assert(info.stats_updates >= 2, "%lu statistics requests", (unsigned long)info.stats_updates);
    kprintf("balloon: statistics sent %lu times\n", (unsigned long)info.stats_updates);
    wait_balloon(&info, true);
    total = read_kb("/dev/meminfo", "MemTotal");
    ktest_assert(total == total0, "MemTotal %lu kB after the deflation, %lu kB before", (unsigned long)total,
                 (unsigned long)total0);
    kprintf("balloon: deflated, MemTotal %lu kB\n", (unsigned long)total);
    /* The QMP script queries the host after the last line. The machine
     * powers off at the end of the test. */
    sleep_ms(2000);
}
KTEST_DEFINE("balloon", test_balloon);

/* With VIRTIO_BALLOON_F_DEFLATE_ON_OOM the allocator takes pages from the
 * balloon. The test allocates 64 MiB more than is free. */
static void test_balloon_oom(void)
{
    struct balloon_info info;
    virtio_balloon_get_info(&info);
    ktest_assert(info.present && info.deflate_on_oom, "no balloon with deflate on OOM");
    kprintf("balloon_oom: ready\n");
    wait_balloon(&info, false);
    uint64_t inflated = info.size;
    struct pmm_stats st;
    pmm_get_stats(&st);
    uint64_t want = st.free_pages + 16384;
    kprintf("balloon_oom: balloon of %lu pages, %lu pages free, allocating %lu pages\n", (unsigned long)inflated,
            (unsigned long)st.free_pages, (unsigned long)want);
    /* The pages form a list through their first word. */
    void *head = NULL;
    uint64_t got = 0;
    for (; got < want; got++) {
        struct page *pg = pmm_alloc_page();
        if (!pg)
            break;
        void **p = P2V(page_to_phys(pg));
        *p = head;
        head = p;
    }
    virtio_balloon_get_info(&info);
    ktest_assert(got == want, "allocation %lu of %lu failed with a balloon of %lu pages", (unsigned long)got,
                 (unsigned long)want, (unsigned long)info.size);
    ktest_assert(info.size <= inflated - 16384 && info.released_on_pressure >= 16384,
                 "balloon of %lu pages, %lu released", (unsigned long)info.size,
                 (unsigned long)info.released_on_pressure);
    kprintf("balloon_oom: %lu pages allocated, the balloon released %lu pages and has %lu\n", (unsigned long)got,
            (unsigned long)info.released_on_pressure, (unsigned long)info.size);
    while (head) {
        void *next = *(void **)head;
        pmm_free_page(phys_to_page(V2P(head)));
        head = next;
    }
    /* The host learns the new size. The balloon remains at it until the
     * host sets a new target. */
    sleep_ms(2000);
    virtio_balloon_get_info(&info);
    ktest_assert(info.size < inflated, "the balloon grew again to %lu pages", (unsigned long)info.size);
    kprintf("balloon_oom: memory freed, balloon of %lu pages reported\n", (unsigned long)info.size);

    /* A program touches 32 MiB more than is free. The case has a swap
     * device. The balloon gives its pages before kswapd evicts a page. */
    uint64_t before = info.size, released = info.released_on_pressure;
    struct swap_stats ss0, ss;
    swap_get_stats(&ss0);
    pmm_get_stats(&st);
    char mib[16];
    ksnprintf(mib, sizeof mib, "%lu", (unsigned long)(st.free_pages / 256 + 32));
    struct proc *p = proc_create_user("/bin/memtouch", (char *const[]){ "memtouch", mib, NULL },
                                      (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(p != NULL, "cannot start memtouch");
    int status = proc_reap(p);
    swap_get_stats(&ss);
    virtio_balloon_get_info(&info);
    ktest_assert(status == 0, "memtouch of %s MiB: status 0x%x", mib, status);
    ktest_assert(ss.swapped_out == ss0.swapped_out, "%lu pages swapped out with a balloon of %lu pages",
                 (unsigned long)(ss.swapped_out - ss0.swapped_out), (unsigned long)info.size);
    ktest_assert(info.released_on_pressure > released && info.size < before, "the balloon released no pages");
    kprintf("balloon_oom: memtouch of %s MiB without swapping, the balloon has %lu pages\n", mib,
            (unsigned long)info.size);
    sleep_ms(2000);
    virtio_balloon_get_info(&info);
    kprintf("balloon_oom: balloon of %lu pages reported\n", (unsigned long)info.size);
    /* The QMP script queries the host after the last line. */
    sleep_ms(2000);
}
KTEST_DEFINE("balloon_oom", test_balloon_oom);
