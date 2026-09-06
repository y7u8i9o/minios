/* The Lua gui module on the compositor: /etc/tests/luagui.lua opens a
 * window whose canvas is one colour, the test finds that colour on the
 * screen and closes the program with Escape. */
#include <tests/ktest.h>
#include <drivers/fbdev.h>
#include <drivers/timer.h>
#include <sched/user.h>
#include <sched/proc.h>
#include <ipc/signal.h>
#include <console.h>
#include "gui_helpers.h"

#define CANVAS 0x0020a040

static void test_gui_lua(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    struct proc *srv = start_server();
    struct proc *cl = proc_create_user("/bin/lua", (char *const[]){ "lua", "/etc/tests/luagui.lua", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start lua");
    int found = 0;
    for (int attempt = 0; attempt < 30 && !found; attempt++) {
        sleep_ms(100);
        for (int y = 0; y < logical_h() && !found; y += 8)
            for (int x = 0; x < logical_w(); x += 8)
                if (pixel(x, y) == CANVAS) {
                    found = 1;
                    break;
                }
    }
    ktest_assert(found, "canvas colour not on screen");
    press_key(0x01);                    /* Escape: the script quits */
    int status = proc_reap(cl);
    ktest_assert(status == 0, "lua status 0x%x", status);
    stop_server(srv);
    kprintf("gui_lua: ok\n");
}
KTEST_DEFINE("gui_lua", test_gui_lua);
