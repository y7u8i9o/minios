#include <tests/ktest.h>
#include <sched/user.h>
#include <sched/proc.h>
#include <lib/cmdline.h>
#include <lib/string.h>
#include <mm/pmm.h>
#include <mm/swap.h>
#include <console.h>
#include <drivers/timer.h>

static long strtol_simple(const char *s)
{
    long v = 0;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v;
}

/* Run the initrd program named by prog=<path> and expect exit status 0.
 * A program under test prints its own diagnostics and exits non zero on
 * failure. Physical memory must be fully returned afterwards. */
static void test_run(void)
{
    char path[128];
    ktest_assert(cmdline_lookup("prog", path, sizeof path) && path[0], "prog= missing");
    struct pmm_stats before, after;
    pmm_get_stats(&before);

    /* args=a,b,c supplies arguments; otherwise two fixed ones. */
    static char args[256];
    char *argv[16] = { path, "arg1", "arg two", NULL };
    if (cmdline_lookup("args", args, sizeof args) && args[0]) {
        int n = 1;
        char *p = args;
        while (*p && n < 15) {
            argv[n++] = p;
            while (*p && *p != ',')
                p++;
            if (*p)
                *p++ = '\0';
        }
        argv[n] = NULL;
    }
    char *const envp[] = { "PATH=/bin", "TEST=1", NULL };
    struct proc *p = proc_create_user(path, argv, envp, &kernel_proc);
    ktest_assert(p != NULL, "cannot start %s", path);
    int status = proc_reap(p);
    kprintf("%s exited with status 0x%x\n", path, status);
    /* status=N on the command line names the expected exit code. */
    char want[16];
    int expected = 0;
    if (cmdline_lookup("status", want, sizeof want) && want[0])
        expected = (int)strtol_simple(want);
    ktest_assert(status == PROC_STATUS_EXITED(expected), "%s failed with status 0x%x", path, status);

    swap_drain();
    /* Another CPU may still be finishing the last switch away from an
     * exited thread; give deferred frees a moment before judging. */
    for (int i = 0; i < 100; i++) {
        pmm_get_stats(&after);
        if (after.free_pages == before.free_pages)
            break;
        sleep_ms(2);
    }
    ktest_assert(after.free_pages == before.free_pages, "leaked %ld pages",
                 (long)before.free_pages - (long)after.free_pages);
    struct swap_stats ss;
    swap_get_stats(&ss);
    ktest_assert(ss.free_slots == ss.total_slots, "leaked %lu swap slots", ss.total_slots - ss.free_slots);
    if (ss.swapped_out)
        kprintf("swap: %lu pages out, %lu in\n", ss.swapped_out, ss.swapped_in);
}
KTEST_DEFINE("run", test_run);
