/* K2 of docs/plan/widgets.md: the widget scenarios of compbench. X12 runs
 * alone, and each scenario is one run of compbench. The program prints the
 * counters of libgui for the scenario. The case gui_bench runs at
 * 1280x800 and gui_bench_hidpi at 2560x1600@2, both on virtio-gpu. The
 * expect files check the counters that do not depend on timing. */
#include <tests/ktest.h>
#include <sched/user.h>
#include <sched/proc.h>
#include <ipc/signal.h>
#include <console.h>
#include "gui_helpers.h"

static void test_gui_bench(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    kprintf("gui_bench: screen %lux%lu scale %u\n", (unsigned long)fb_screen.width, (unsigned long)fb_screen.height,
            (unsigned)(fb_screen_scale ? fb_screen_scale : 1));
    struct proc *srv = proc_create_user("/bin/x12", (char *const[]){ "x12", "-s", NULL }, (char *const[]){ NULL },
                                        &kernel_proc);
    ktest_assert(srv != NULL, "cannot start the compositor");
    ktest_wait_idle(1200);
    const char *scenarios[] = { "wheel", "type", "status", "edit", "scroll", "hover" };
    for (unsigned i = 0; i < sizeof scenarios / sizeof scenarios[0]; i++) {
        struct proc *p = proc_create_user("/bin/compbench", (char *const[]){ "compbench", (char *)scenarios[i], NULL },
                                          (char *const[]){ NULL }, &kernel_proc);
        ktest_assert(p != NULL, "cannot start compbench %s", scenarios[i]);
        int status = proc_reap(p);
        ktest_assert(status == 0, "compbench %s status 0x%x", scenarios[i], status);
    }
    signal_send(srv, SIGTERM);
    int status = proc_reap(srv);
    ktest_assert(status == 0, "compositor status 0x%x", status);
    kprintf("gui_bench: ok\n");
}
KTEST_DEFINE("gui_bench", test_gui_bench);
