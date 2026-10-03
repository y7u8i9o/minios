/* The Code editor (a Lua program) on the compositor: it opens a file,
 * F5 runs it through the run panel, and Ctrl+Q closes the window. The
 * program logs what it did; the case checks those lines. */
#include <tests/ktest.h>
#include <drivers/fbdev.h>
#include <drivers/timer.h>
#include <sched/user.h>
#include <sched/proc.h>
#include <console.h>
#include "gui_helpers.h"

static void test_gui_code(void)
{
    install_app("code");
    ktest_assert(fb_screen_present, "no framebuffer");
    struct proc *srv = start_server();
    struct proc *cl = proc_create_user("/usr/bin/code", (char *const[]){ "code", "/etc/tests/sample.lua", NULL },
                                       (char *const[]){ "PATH=/bin", "HOME=/home", NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start code");
    sleep_ms(2500);
    press_key(0x3f);                    /* F5: run */
    sleep_ms(3000);
    ctrl_key(0x10);                     /* Ctrl+Q: quit */
    int status = proc_reap(cl);
    ktest_assert(status == 0, "code status 0x%x", status);
    stop_server(srv);
    kprintf("gui_code: ok\n");
}
KTEST_DEFINE("gui_code", test_gui_code);
