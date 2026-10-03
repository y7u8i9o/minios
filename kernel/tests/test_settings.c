/* The settings program: every page opens on the desktop and closes with
 * Alt+F4 without a crash, and the X12 tool does the same. A page named by
 * page=<name> on the command line is opened alone and held open for
 * screenshots. */
#include <tests/ktest.h>
#include <sched/user.h>
#include <sched/proc.h>
#include <ipc/signal.h>
#include <drivers/timer.h>
#include <lib/cmdline.h>
#include <lib/string.h>
#include <console.h>
#include "gui_helpers.h"

static const char *const pages[] = { "appearance", "display", "keyboard", "mouse", "sound", "time", "filetypes", "launcher",
                                     "system", "region", "users" };

static int windows;             /* toplevels created so far: the server cascades by 30 px each, 8 places */

static void open_and_close(const char *path, const char *arg0, const char *arg1, int hold_ms)
{
    int wx = 40 + windows % 8 * 30, wy = 60 + windows % 8 * 30;
    windows++;
    struct proc *cl = proc_create_user(path, (char *const[]){ (char *)arg0, (char *)arg1, NULL }, (char *const[]){ NULL },
                                       &kernel_proc);
    ktest_assert(cl != NULL, "cannot start %s", path);
    sleep_ms(hold_ms);
    /* A page that reads more data, or a host running several guests,
     * may map the window later than hold_ms. */
    for (int waited = 0; pixel(wx + 2, wy - 10) != 0x00ebebeb && waited < 5000; waited += 100)
        sleep_ms(100);
    kprintf("gui_settings: %s %s shown\n", arg0, arg1 ? arg1 : "");
    ktest_assert(pixel(wx + 2, wy - 10) == 0x00ebebeb, "%s %s window has an active title bar: %08x", arg0,
                 arg1 ? arg1 : "", pixel(wx + 2, wy - 10));
    alt_key(0x3e);
    int status = proc_reap(cl);
    ktest_assert(status == 0, "%s %s status 0x%x", arg0, arg1 ? arg1 : "", status);
}

static void test_gui_settings(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    struct proc *srv = start_server();
    struct proc *desktop = proc_create_user("/bin/desktop", (char *const[]){ "desktop", NULL }, (char *const[]){ NULL },
                                            &kernel_proc);
    ktest_assert(desktop != NULL, "cannot start the desktop");
    sleep_ms(1500);
    char page[32];
    if (cmdline_lookup("page", page, sizeof page) && page[0]) {
        if (strcmp(page, "x12") == 0)
            open_and_close("/bin/x12settings", "x12settings", NULL, 12000);
        else
            open_and_close("/bin/settings", "settings", page, 12000);
    } else {
        for (size_t i = 0; i < ARRAY_SIZE(pages); i++)
            open_and_close("/bin/settings", "settings", pages[i], 1500);
        open_and_close("/bin/x12settings", "x12settings", NULL, 1500);
    }
    signal_send(desktop, SIGTERM);
    proc_reap(desktop);
    stop_server(srv);
    kprintf("gui_settings: ok\n");
}
KTEST_DEFINE("gui_settings", test_gui_settings);
