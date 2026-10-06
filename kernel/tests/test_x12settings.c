/* x12settings (docs/plan/x12settings.md, docs/design/x12settings.md). */
#include <tests/ktest.h>
#include <sched/user.h>
#include <sched/proc.h>
#include <ipc/signal.h>
#include <drivers/timer.h>
#include "gui_helpers.h"

static struct proc *start(const char *path, char *const argv[])
{
    struct proc *p = proc_create_user(path, argv, (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(p != NULL, "cannot start %s", path);
    return p;
}

static void run(const char *path, char *const argv[])
{
    int status = proc_reap(start(path, argv));
    ktest_assert(status == 0, "%s status 0x%x", path, status);
}

/* X2: comptest makes 10 commits with damage and 10 with a frame callback
 * only. compstat -h then prints the recorded frames. compbench animates
 * its window for one second while the Performance page of x12settings
 * is open. The page prints the frames of each second that had frames.
 * The qmp script takes a screendump of the page. */
static void test_x12settings_perf(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    struct proc *srv = start("/bin/x12", (char *const[]){ "x12", "-s", NULL });
    ktest_wait_idle(1200);
    run("/bin/comptest", (char *const[]){ "comptest", "damage", NULL });
    run_shell("compstat -h");
    struct proc *bench = start("/bin/compbench", (char *const[]){ "compbench", "anim", NULL });
    ktest_wait_idle(400);
    struct proc *tool = start("/bin/x12settings", (char *const[]){ "x12settings", NULL });
    int status = proc_reap(bench);
    ktest_assert(status == 0, "compbench status 0x%x", status);
    ktest_wait_idle(2500);
    kprintf("x12settings_perf: page shown\n");
    sleep_ms(1500);
    alt_key(0x3e);
    status = proc_reap(tool);
    ktest_assert(status == 0, "x12settings status 0x%x", status);
    signal_send(srv, SIGTERM);
    status = proc_reap(srv);
    ktest_assert(status == 0, "x12 status 0x%x", status);
    kprintf("x12settings_perf: ok\n");
}
KTEST_DEFINE("x12settings_perf", test_x12settings_perf);
