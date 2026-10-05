/* R6: the live medium. The kernel boots from the ISO image with
 * root=LABEL=MINIOS_LIVE, and init of the live tree starts the greeter
 * with -a live. The session of the account live (uid 1000) starts without
 * a login. doas permits live to start the graphical installer without a
 * password, and the launcher menu contains the entry "Install minios". */
#include <tests/ktest.h>
#include <sched/user.h>
#include <sched/proc.h>
#include <drivers/timer.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <lib/printf.h>
#include <console.h>
#include "gui_helpers.h"

static void test_live_session(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    struct proc *p = proc_create_user("/bin/init", (char *const[]){ "/bin/init", NULL },
                                      (char *const[]){ "PATH=/bin", NULL }, &kernel_proc);
    ktest_assert(p != NULL, "cannot start /bin/init");
    proc_set_init(p);
    ktest_assert(wait_procs("panel", 1000, 1, 60000), "no panel of uid 1000");
    ktest_assert(wait_procs("desktop", 1000, 1, 10000), "no desktop of uid 1000");
    ktest_assert(count_procs("login", -1) == 0, "the console login runs");
    kprintf("live: session of live\n");

    /* doas -C checks the rule for live without running the command, and
     * the shell prints the result and the entry of the launcher menu. */
    struct proc *c = proc_create_user("/bin/sh", (char *const[]){ "sh", "-c",
                                      "doas -u live doas -C /etc/doas.conf /usr/bin/installer-gui --session"
                                      " | sed 's/^/live: doas rule /' && grep '^Install minios=' /etc/launcher"
                                      " | sed 's/^/live: launcher /'", NULL },
                                      (char *const[]){ "PATH=/bin:/usr/bin", NULL }, &kernel_proc);
    ktest_assert(c != NULL, "cannot start sh");
    int status = proc_reap(c);
    ktest_assert(status == 0, "the checks of live, status 0x%x", status);
    ktest_pass();
}
KTEST_DEFINE("live_session", test_live_session);
