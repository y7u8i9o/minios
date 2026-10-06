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

/* Pixels of a colour on the row y between x0 and x1, logical pixels. */
static int count_on_row(int y, int x0, int x1, uint32_t color)
{
    int n = 0;
    for (int x = x0; x < x1; x++)
        n += pixel(x, y) == color;
    return n;
}

/* X3 to X5: the clients and the capture of a comptest window from the
 * command line, the outline of the window, the debug views of X12, and
 * the Clients and Settings pages for the screendumps of the qmp script.
 * comptest maps the window "seat" of 200x150 pixels in 0x00c8f0c8 as the
 * first toplevel, at (40,60). */
static void test_x12settings_views(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    int sw = logical_w(), sh = logical_h();
    struct proc *srv = start("/bin/x12", (char *const[]){ "x12", "-s", NULL });
    ktest_wait_idle(1200);
    struct proc *win = start("/bin/comptest", (char *const[]){ "comptest", "seat", NULL });
    ktest_wait_idle(1500);
    ktest_assert(pixel(140, 135) == 0x00c8f0c8, "the comptest window is not shown: %06x", pixel(140, 135));
    run("/bin/x12settings", (char *const[]){ "x12settings", "clients", NULL });
    run("/bin/x12settings", (char *const[]){ "x12settings", "capture", "seat", NULL });

    /* The outline lies on the frame of the window while the tool runs. */
    struct proc *outline = start("/bin/x12settings", (char *const[]){ "x12settings", "highlight", "seat", "3", NULL });
    ktest_wait_idle(1200);
    int magenta = count_on_row(135, 20, 260, 0x00ff00ff);
    kprintf("x12settings_views: %d outline pixels on the row\n", magenta);
    ktest_assert(magenta >= 4, "no outline: %d pixels", magenta);
    int status = proc_reap(outline);
    ktest_assert(status == 0, "x12settings highlight status 0x%x", status);
    ktest_wait_idle(500);
    ktest_assert(count_on_row(135, 20, 260, 0x00ff00ff) == 0, "the outline remains after the exit of the tool");

    /* Opaque regions turn green. */
    run_shell("x12settings set debug_opaque 1");
    ktest_wait_idle(300);
    uint32_t tinted = pixel(140, 135);
    kprintf("x12settings_views: opaque tint %06x\n", tinted);
    ktest_assert(tinted != 0x00c8f0c8 && (tinted >> 8 & 0xff) > (tinted >> 16 & 0xff), "no green tint: %06x", tinted);
    run_shell("x12settings set debug_opaque 0");
    ktest_wait_idle(300);
    ktest_assert(pixel(140, 135) == 0x00c8f0c8, "the tint remains: %06x", pixel(140, 135));

    /* The frame counter. */
    run_shell("x12settings set debug_fps 1");
    sleep_ms(2500);
    ktest_assert(pixel(sw - 86, 5) == 0x00202020, "no frame counter: %06x", pixel(sw - 86, 5));
    run_shell("x12settings set debug_fps 0");
    ktest_wait_idle(300);
    ktest_assert(pixel(sw - 86, 5) == 0x00306080, "the frame counter remains: %06x", pixel(sw - 86, 5));

    /* A new desktop colour damages the screen, which flashes red. */
    run_shell("x12settings set debug_damage 1");
    ktest_wait_idle(300);
    struct proc *setter =
        start("/bin/x12settings", (char *const[]){ "x12settings", "set", "desktop_color", "3170433", NULL });
    uint32_t flash = 0;
    uint64_t t0 = timer_ms();
    while (timer_ms() - t0 < 3000 && !flash) {
        uint32_t c = pixel(sw - 10, sh - 10);
        if ((c >> 16 & 0xff) > (c & 0xff))
            flash = c;
    }
    kprintf("x12settings_views: flash %06x\n", flash);
    status = proc_reap(setter);
    ktest_assert(status == 0, "x12settings set status 0x%x", status);
    ktest_assert(flash, "no red flash: %06x", pixel(sw - 10, sh - 10));
    sleep_ms(800);
    ktest_wait_idle(300);
    ktest_assert(pixel(sw - 10, sh - 10) == 0x00306081, "the flash remains: %06x", pixel(sw - 10, sh - 10));
    run_shell("x12settings set debug_damage 0");

    /* The pages for the screendumps. The window of x12settings lies at
     * (70,90), its tab titles on the row 107. A click on the fourth row
     * of the tree selects the surface of comptest. */
    struct proc *tool = start("/bin/x12settings", (char *const[]){ "x12settings", NULL });
    ktest_wait_idle(2500);
    int cx = sw / 2, cy = sh / 2;
    mouse_move_to(&cx, &cy, 219, 107, 0);
    mouse_click(1);
    ktest_wait_idle(1500);
    mouse_move_to(&cx, &cy, 160, 209, 0);
    mouse_click(1);
    ktest_wait_idle(2500);
    kprintf("x12settings_views: clients shown\n");
    sleep_ms(1500);
    mouse_move_to(&cx, &cy, 291, 107, 0);
    mouse_click(1);
    ktest_wait_idle(1500);
    kprintf("x12settings_views: settings shown\n");
    sleep_ms(1500);
    alt_key(0x3e);
    status = proc_reap(tool);
    ktest_assert(status == 0, "x12settings status 0x%x", status);
    alt_key(0x3e);
    proc_reap(win);
    signal_send(srv, SIGTERM);
    status = proc_reap(srv);
    ktest_assert(status == 0, "x12 status 0x%x", status);
    kprintf("x12settings_views: ok\n");
}
KTEST_DEFINE("x12settings_views", test_x12settings_views);
