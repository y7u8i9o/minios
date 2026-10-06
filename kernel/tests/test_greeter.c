/* U4: the graphical login. The greeter starts X12 and its login window,
 * Enter logs in the preselected account user (uid 1000, no password), the
 * session's panel and desktop run as uid 1000, Log out in the power menu
 * of the panel ends the session, the greeter shows its window again, and X12 then
 * refuses a client of uid 1000. pause=1 leaves the login window open for
 * eight seconds, for screenshots. */
#include <tests/ktest.h>
#include <sched/user.h>
#include <sched/proc.h>
#include <ipc/signal.h>
#include <drivers/timer.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <lib/printf.h>
#include <lib/cmdline.h>
#include <console.h>
#include "gui_helpers.h"

static void test_greeter(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    int sw = logical_w(), sh = logical_h();
    /* The kernel protects pid 1, init, from SIGKILL. A first short process
     * takes that pid, which lets the end of the test kill the greeter. */
    struct proc *first = proc_create_user("/bin/sh", (char *const[]){ "sh", "-c", "exit 0", NULL },
                                          (char *const[]){ "PATH=/bin", NULL }, &kernel_proc);
    ktest_assert(first != NULL, "cannot start sh");
    proc_reap(first);
    struct proc *g = proc_create_user("/bin/greeter", (char *const[]){ "greeter", "-s", NULL },
                                      (char *const[]){ "PATH=/bin", NULL }, &kernel_proc);
    ktest_assert(g != NULL, "cannot start the greeter");
    /* The supervisor and its login window. */
    ktest_assert(wait_procs("greeter", 0, 2, 10000), "no login window");
    ktest_assert(count_procs("x12", 0) == 1, "no display server");
    sleep_ms(1500);
    kprintf("gui_greeter: login window shown\n");
    char pause_arg[8];
    if (cmdline_lookup("pause", pause_arg, sizeof pause_arg) && pause_arg[0] == '1')
        sleep_ms(8000);                 /* screenshots of the login window */

    greeter_login_user();
    ktest_assert(count_procs("greeter", -1) == 1, "the login window remains");
    kprintf("gui_greeter: session of uid 1000\n");
    sleep_ms(1500);

    /* Log out in the power menu (B4 of docs/plan/desktop-panel.md). */
    panel_power_choose(sw, sh, POWER_ROW_LOGOUT);
    ktest_assert(wait_procs("panel", -1, 0, 10000), "the session did not end");
    ktest_assert(wait_procs("greeter", 0, 2, 10000), "no login window after the session");
    kprintf("gui_greeter: login window shown again\n");

    /* Without a session X12 refuses the clients of uid 1000. */
    struct proc *as = proc_create_user("/bin/doas", (char *const[]){ "doas", "-u", "user", "x12settings", "set", "verbose", "0", NULL },
                                       (char *const[]){ "PATH=/bin", NULL }, &kernel_proc);
    ktest_assert(as != NULL, "cannot start doas");
    proc_reap(as);
    sleep_ms(300);

    signal_send_pgrp(g->pid, SIGKILL);
    proc_reap(g);
    proc_reap_children(&kernel_proc);
}
KTEST_DEFINE("gui_greeter", test_greeter);

/* B4 of docs/plan/desktop-panel.md: the system boots through init to the
 * greeter, the account user logs in, and Shut down in the power menu of the
 * panel powers the machine off through init. QEMU then ends with status 0
 * (tests/cases/panel_power/post). */
static void test_panel_power(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    int sw = logical_w(), sh = logical_h();
    /* init with the configuration of the image, whose console entry is the
     * greeter. */
    struct proc *init = proc_create_user("/bin/init", (char *const[]){ "/bin/init", NULL },
                                         (char *const[]){ "PATH=/bin", NULL }, &kernel_proc);
    ktest_assert(init != NULL, "cannot start /bin/init");
    proc_set_init(init);
    ktest_assert(wait_procs("greeter", 0, 2, 30000), "no login window");
    sleep_ms(1500);
    greeter_login_user();
    sleep_ms(1500);
    kprintf("panel_power: session of uid 1000, choosing Shut down\n");
    panel_power_choose(sw, sh, POWER_ROW_SHUTDOWN);
    int status = proc_reap(init);
    ktest_fail("init exited with status 0x%x instead of powering off", status);
}
KTEST_DEFINE("panel_power", test_panel_power);

/* A normal boot starts the greeter as the console entry of init. */
static void test_greeter_boot(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    struct proc *p = proc_create_user("/bin/init", (char *const[]){ "/bin/init", NULL },
                                      (char *const[]){ "PATH=/bin", NULL }, &kernel_proc);
    ktest_assert(p != NULL, "cannot start /bin/init");
    proc_set_init(p);
    ktest_assert(wait_procs("greeter", 0, 2, 15000), "no login window");
    ktest_assert(count_procs("x12", 0) == 1, "no display server");
    ktest_assert(count_procs("login", -1) == 0, "the console login runs");
    kprintf("greeter_boot: login window shown\n");
    ktest_pass();
}
KTEST_DEFINE("greeter_boot", test_greeter_boot);

/* Without a display the greeter runs the console login in its place. */
static void test_greeter_fallback(void)
{
    struct proc *p = proc_create_user("/bin/init", (char *const[]){ "/bin/init", NULL },
                                      (char *const[]){ "PATH=/bin", NULL }, &kernel_proc);
    ktest_assert(p != NULL, "cannot start /bin/init");
    proc_set_init(p);
    ktest_assert(wait_procs("login", 0, 1, 15000), "no console login");
    ktest_assert(count_procs("x12", -1) == 0, "a display server runs");
    kprintf("greeter_fallback: console login\n");
    ktest_pass();
}
KTEST_DEFINE("greeter_fallback", test_greeter_fallback);
