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
    swap_drain();
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
    /* netpeer_port=N (set by the boot harness for a native host peer) is
     * passed to the program as NETPEER_PORT. */
    static char peer[64] = "NETPEER_PORT=";
    char *const envp[] = { "PATH=/bin", "TEST=1",
                           cmdline_lookup("netpeer_port", peer + 13, sizeof peer - 13) ? peer : NULL,
                           NULL };
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
    /* Children the program did not wait for were adopted by the kernel
     * process; their memory must be returned before it is counted. */
    proc_reap_children(&kernel_proc);

    /* resident_pages=N allows the program to leave up to N pages to objects of
     * the kernel that last until shutdown, such as the partitions of a
     * disk it partitioned (docs/design/block.md). */
    char resident[16];
    long allowed = cmdline_lookup("resident_pages", resident, sizeof resident) && resident[0] ? (long)strtol_simple(resident) : 0;
    swap_drain();
    /* Another CPU may still be finishing the last switch away from an
     * exited thread; give deferred frees a moment before judging. */
    for (int i = 0; i < 100; i++) {
        swap_drain();
        pmm_get_stats(&after);
        if ((long)before.free_pages - (long)after.free_pages <= allowed)
            break;
        sleep_ms(2);
    }
    ktest_assert((long)before.free_pages - (long)after.free_pages <= allowed, "leaked %ld pages",
                 (long)before.free_pages - (long)after.free_pages);
    struct swap_stats ss;
    swap_get_stats(&ss);
    ktest_assert(ss.free_slots == ss.total_slots, "leaked %lu swap slots", ss.total_slots - ss.free_slots);
    if (ss.swapped_out)
        kprintf("swap: %lu pages out, %lu in\n", ss.swapped_out, ss.swapped_in);
}
KTEST_DEFINE("run", test_run);
