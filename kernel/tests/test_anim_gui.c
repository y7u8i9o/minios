/* The animations of X12 (docs/plan/animation-gpu.md, docs/design/
 * animations.md). X12 runs with the panel and the setting anim_percent
 * 400. Every animation is therefore four times slower than normal. comptest
 * shell maps a server decorated window of 200x150 pixels in 0x00dcdcdc.
 * Its frame cascades to (40,30), and its contents begin at (41,59). The
 * test polls the centre of the contents during each animation and
 * requires a frame between the two end states, then the final state:
 * the map (fade in and growth), the minimize into the button of the
 * panel, the restore from the button and the close (fade out of a
 * snapshot). */
#include <tests/ktest.h>
#include <sched/user.h>
#include <sched/proc.h>
#include <ipc/signal.h>
#include <drivers/timer.h>
#include <console.h>
#include "gui_helpers.h"

#define DESKTOP 0x00306080u
#define CONTENTS 0x00dcdcdcu
#define CENTRE_X (41 + 100)
#define CENTRE_Y (59 + 75)

/* Each colour channel of v lies strictly between the channels of a and b
 * where those differ: a blend of a and b with an alpha between 0 and 255. */
static bool strictly_between(uint32_t v, uint32_t a, uint32_t b)
{
    for (int sh = 0; sh < 24; sh += 8) {
        uint32_t x = v >> sh & 0xff, lo = a >> sh & 0xff, hi = b >> sh & 0xff;
        if (lo > hi) {
            uint32_t t = lo;
            lo = hi;
            hi = t;
        }
        if (lo != hi && (x <= lo || x >= hi))
            return false;
    }
    return true;
}

/* Poll the centre of the window every few milliseconds until it shows
 * end, for at most 3 s. Returns the first value that blends from and end
 * in every channel, else the first value other than from and end, or 0
 * when no frame between the end states appeared. The first frames of a
 * fade may change one channel only. The test fails when the centre never
 * shows end. */
static uint32_t transition(const char *what, uint32_t from, uint32_t end)
{
    uint32_t between = 0, blend = 0;
    uint64_t t0 = timer_ms();
    for (;;) {
        uint32_t v = pixel(CENTRE_X, CENTRE_Y);
        if (v == end)
            break;
        if (v != from && !between)
            between = v;
        if (!blend && strictly_between(v, from, end))
            blend = v;
        ktest_assert(timer_ms() - t0 < 3000, "%s: the end state %06x is not reached, last %06x", what, end, v);
        sleep_ms(4);
    }
    uint32_t shown = blend ? blend : between;
    kprintf("x12_anim: %s from %06x over %06x to %06x in %lu ms\n", what, from, shown, end,
            (unsigned long)(timer_ms() - t0));
    return shown;
}

static void test_x12_anim(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    int sw = logical_w(), sh = logical_h();
    struct proc *srv = start_server();
    x12_setting("anim_percent", "400");
    ktest_wait_idle(300);
    int cx = sw / 2, cy = sh / 2;

    /* The map: the alpha grows from 0. A frame therefore shows a blend of
     * the desktop and the contents. */
    struct proc *cl = proc_create_user("/bin/comptest", (char *const[]){ "comptest", "shell", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start comptest");
    uint32_t v = transition("map", DESKTOP, CONTENTS);
    ktest_assert(strictly_between(v, DESKTOP, CONTENTS), "no frame of the map between the end states: %06x", v);
    ktest_wait_idle(300);

    /* The minimize button of the decorations: the window shrinks into its
     * button in the panel and fades out. */
    mouse_move_to(&cx, &cy, 41 + 200 - 16 - 2 * 18 + 7, 59 - 28 + 7 + 7, 0);
    kprintf("x12_anim: minimize starts\n");
    mouse_click_now(1);
    v = transition("minimize", CONTENTS, DESKTOP);
    ktest_assert(v, "no frame of the minimize between the end states");
    ktest_wait_idle(300);

    /* The button of the window in the panel restores it. */
    mouse_move_to(&cx, &cy, PANEL_TASKS_X + 20, PANEL_ROW(sh), 0);
    mouse_click_now(1);
    v = transition("restore", DESKTOP, CONTENTS);
    ktest_assert(v, "no frame of the restore between the end states");
    ktest_wait_idle(300);

    /* Alt+F4 closes the window. A snapshot of the window fades out. */
    alt_key_now(0x3e);
    v = transition("close", CONTENTS, DESKTOP);
    ktest_assert(strictly_between(v, CONTENTS, DESKTOP), "no frame of the close between the end states: %06x", v);
    int status = proc_reap(cl);
    ktest_assert(status == 0, "comptest status 0x%x", status);
    ktest_wait_idle(300);
    run_shell("compstat");
    stop_server(srv);
    kprintf("x12_anim: ok\n");
}
KTEST_DEFINE("x12_anim", test_x12_anim);
