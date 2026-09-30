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

/* This test covers the image, clipboard and layer bindings.
 * /etc/tests/luabind.lua checks images, a clipboard round trip with a
 * second client and a layer window, and logs each step. The test finds
 * the enlarged image of its canvas (blue left of orange) and the purple
 * layer window along the top edge, then presses Escape, which closes
 * the layer and ends the program; its status is 0 when every check
 * passed. */
#define SWATCH_LEFT 0x002060c0
#define SWATCH_RIGHT 0x00c06020
#define LAYER 0x007030a0

static int find_swatch(void)
{
    for (int y = 0; y < logical_h(); y += 4)
        for (int x = 0; x + 8 < logical_w(); x += 4)
            if (pixel(x, y) == SWATCH_LEFT)
                for (int r = x + 4; r < logical_w(); r += 4)
                    if (pixel(r, y) == SWATCH_RIGHT)
                        return 1;
    return 0;
}

static void test_gui_lua_bindings(void)
{
    ktest_assert(fb_screen_present, "the machine has no framebuffer");
    struct proc *srv = start_server();
    struct proc *cl = proc_create_user("/bin/lua", (char *const[]){ "lua", "/etc/tests/luabind.lua", NULL },
                                       (char *const[]){ "PATH=/bin", NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "lua did not start");
    int layer = 0;
    for (int attempt = 0; attempt < 300 && !layer; attempt++) {
        sleep_ms(100);
        layer = pixel(logical_w() / 2, 4) == LAYER && pixel(4, 24) == LAYER;
    }
    ktest_assert(layer, "the layer window is not on the screen");
    ktest_assert(find_swatch(), "the enlarged image is not on the screen");
    press_key(0x01);                    /* Escape closes the layer and quits. */
    int status = proc_reap(cl);
    ktest_assert(status == 0, "lua exited with status 0x%x", status);
    sleep_ms(300);
    ktest_assert(pixel(logical_w() / 2, 4) != LAYER, "the layer window is still on the screen");
    stop_server(srv);
    kprintf("gui_lua_bindings: images, the clipboard and layer windows work\n");
}
KTEST_DEFINE("gui_lua_bindings", test_gui_lua_bindings);
