/* The compositor's pointer acceleration and libgui's key repeat, driven
 * through the PS/2 drivers with the compositor running. */
#include <tests/ktest.h>
#include <drivers/ps2mouse.h>
#include <drivers/ps2kbd.h>
#include <drivers/fbdev.h>
#include <drivers/timer.h>
#include <sched/user.h>
#include <sched/proc.h>
#include <ipc/signal.h>
#include <fs/vfs.h>
#include <console.h>
#include "gui_helpers.h"

static void expect_cursor(int x, int y, const char *what)
{
    ktest_assert(arrow_cursor_at(x, y), "%s: the cursor is not at %d,%d", what, x, y);
}

static void set_setting(const char *key, const char *value)
{
    struct proc *p = proc_create_user("/bin/x12settings", (char *const[]){ "x12settings", "set", (char *)key, (char *)value, NULL },
                                      (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(p != NULL, "cannot start x12settings");
    ktest_assert(proc_reap(p) == 0, "x12settings set %s failed", key);
    ktest_wait_idle(200);
}

/* Relative motion of the PS/2 mouse: the adaptive profile scales slow
 * motion by 0.8 and fast motion by 2.8; the flat profile with the speed
 * at 100 scales by 3; fractions accumulate below one pixel. */
static void test_gui_pointer(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    struct proc *srv = proc_create_user("/bin/x12", (char *const[]){ "x12", "-s", NULL }, (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(srv != NULL, "cannot start the compositor");
    ktest_wait_idle(1200);
    int sw = logical_w(), sh = logical_h();
    int x = sw / 2, y = sh / 2;
    expect_cursor(x, y, "start");

    /* Ten packets of one unit, 20 ms apart: below one unit per ms. */
    for (int i = 0; i < 10; i++) {
        feed_packet(0, 1, 0);
        sleep_ms(20);
    }
    ktest_wait_idle(200);
    x += 8;
    expect_cursor(x, y, "slow motion");
    /* Five packets of twenty units back to back: the first one comes
     * after a pause and is slow, the next four are above eight units
     * per ms. */
    for (int i = 0; i < 5; i++)
        feed_packet(0, 20, 0);
    ktest_wait_idle(200);
    x += 16 + 4 * 56;
    expect_cursor(x, y, "fast motion");

    set_setting("pointer_accel", "0");
    set_setting("pointer_speed", "100");
    feed_packet(0, -10, 0);
    ktest_wait_idle(200);
    x -= 30;
    expect_cursor(x, y, "flat profile at speed 100");

    /* The cursor sits at the centre of its pixel: two packets of 0.2
     * pixels remain inside it, the third crosses into the next one. */
    set_setting("pointer_speed", "-100");
    feed_packet(0, 0, -1);            /* down by 0.2 pixels */
    feed_packet(0, 0, -1);
    ktest_wait_idle(200);
    expect_cursor(x, y, "fractions below one pixel");
    feed_packet(0, 0, -1);
    ktest_wait_idle(200);
    y += 1;
    expect_cursor(x, y, "fractions reaching one pixel");
    ktest_assert(pixel(sw / 2, sh / 2) == 0x00306080, "start position repainted: %08x", pixel(sw / 2, sh / 2));
    kprintf("gui_pointer: acceleration ok\n");
    signal_send(srv, SIGTERM);
    int status = proc_reap(srv);
    ktest_assert(status == 0, "compositor status 0x%x", status);
}
KTEST_DEFINE("gui_pointer", test_gui_pointer);

/* A key pressed in a libgui window repeats after the delay: evtest logs
 * the repeated press. */
static void test_gui_repeat(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    struct proc *srv = start_server();
    struct proc *cl = proc_create_user("/bin/evtest", (char *const[]){ "evtest", NULL }, (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start evtest");
    ktest_wait_idle(1500);
    int cx = logical_w() / 2, cy = logical_h() / 2;
    mouse_move_to(&cx, &cy, 40 + 100, 60 + 40, 0);
    mouse_click(1);
    ktest_wait_idle(300);
    ps2kbd_feed_scancode(0x1e);
    ktest_wait_idle(900);
    ps2kbd_feed_scancode(0x9e);
    ktest_wait_idle(300);
    alt_key(0x3e);
    int status = proc_reap(cl);
    ktest_assert(status == 0, "evtest status 0x%x", status);
    stop_server(srv);
    kprintf("gui_repeat: key repeat ok\n");
}
KTEST_DEFINE("gui_repeat", test_gui_repeat);
