/* The benchmark of the graphical session (docs/design/graphics-performance.md).
 * X12 runs alone. The first part drives a compbench window from here:
 * drag moves it with 200 pointer packets on its title bar, pointer moves
 * the pointer 400 times over it, and resize changes its size 20 times by
 * a drag of its bottom right corner. compstat prints the frame statistics of each of these
 * scenarios. The second part runs the client scenarios of compbench,
 * which print their statistics themselves. The case comp_bench runs at
 * 1280x800 and comp_bench_hidpi at 2560x1600@2, both on virtio-gpu. The
 * values are printed for the record. The expect files check only the
 * counters that do not depend on timing. */
#include <tests/ktest.h>
#include <sched/user.h>
#include <sched/proc.h>
#include <ipc/signal.h>
#include <drivers/timer.h>
#include <console.h>
#include "gui_helpers.h"

/* The compbench window: 760x540 logical pixels of contents, which X12
 * places at (40,60) as its first toplevel. */
#define WIN_X 40
#define WIN_Y 60
#define WIN_W 760
#define WIN_H 540

static void print_usage(const char *scenario)
{
    unsigned long x12_ms = 0, x12_kb = 0, bench_ms = 0, bench_kb = 0;
    proc_table_find("x12", -1, &x12_ms, &x12_kb);
    proc_table_find("compbench", -1, &bench_ms, &bench_kb);
    kprintf("comp_bench: %s usage x12 %lu ms %lu KiB, compbench %lu ms %lu KiB\n", scenario, x12_ms, x12_kb, bench_ms,
            bench_kb);
}

static void run_scenario(const char *scenario)
{
    struct proc *p = proc_create_user("/bin/compbench", (char *const[]){ "compbench", (char *)scenario, NULL },
                                      (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(p != NULL, "cannot start compbench %s", scenario);
    int status = proc_reap(p);
    ktest_assert(status == 0, "compbench %s status 0x%x", scenario, status);
}

/* The active title bar of the window at its place. */
static bool window_at_home(void)
{
    return pixel(WIN_X + 2, WIN_Y - 10) == 0x00ebebeb;
}

static void scenario_drag(int *cx, int *cy)
{
    mouse_move_to(cx, cy, WIN_X + 300, WIN_Y - 10, 0);
    ktest_wait_idle(200);
    run_shell("compstat -r");
    feed_packet(1, 0, 0);
    for (int i = 0; i < 100; i++) {
        feed_packet(1, 1, 0);
        sleep_ms(4);
    }
    for (int i = 0; i < 100; i++) {
        feed_packet(1, -1, 0);
        sleep_ms(4);
    }
    feed_packet(0, 0, 0);
    int settled = 0;
    for (int i = 0; i < 200 && !settled; i++) {
        sleep_ms(10);
        settled = window_at_home();
    }
    ktest_assert(settled, "the window did not return to its place");
    ktest_wait_idle(300);
    kprintf("comp_bench: drag done\n");
    run_shell("compstat");
    print_usage("drag");
}

static void scenario_pointer(int *cx, int *cy)
{
    run_shell("compstat -r");
    for (int i = 0; i < 400; i++)
        mouse_move_to(cx, cy, 100 + (i % 20) * 30, 100 + (i % 13) * 25, 0);
    ktest_wait_idle(300);
    kprintf("comp_bench: pointer done\n");
    run_shell("compstat");
    print_usage("pointer");
}

/* The resize border lies in the shadow outside the frame. X12 applies a
 * resize at the release of the button, so every step is one drag of the
 * corner, alternately 60x40 pixels inwards and back. */
static void scenario_resize(int *cx, int *cy)
{
    int x = WIN_X + WIN_W + 3, y = WIN_Y + WIN_H + 3;
    mouse_move_to(cx, cy, x, y, 0);
    ktest_wait_idle(200);
    run_shell("compstat -r");
    for (int i = 0; i < 20; i++) {
        int smaller = i % 2 == 0;
        /* The client receives no motion during the grab of the previous
         * drag. A motion before the press gives it the position. */
        int px = *cx, py = *cy;
        mouse_move_to(cx, cy, px + 1, py + 1, 0);
        mouse_move_to(cx, cy, px, py, 0);
        feed_packet(1, 0, 0);
        ktest_wait_idle(150);
        mouse_move_to(cx, cy, smaller ? x - 60 : x, smaller ? y - 40 : y, 1);
        ktest_wait_idle(100);
        feed_packet(0, 0, 0);
        ktest_wait_idle(300);
    }
    ktest_wait_idle(300);
    kprintf("comp_bench: resize done\n");
    run_shell("compstat");
    print_usage("resize");
}

static void test_comp_bench(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    kprintf("comp_bench: screen %lux%lu scale %u\n", (unsigned long)fb_screen.width, (unsigned long)fb_screen.height,
            (unsigned)(fb_screen_scale ? fb_screen_scale : 1));
    struct proc *srv = proc_create_user("/bin/x12", (char *const[]){ "x12", "-s", NULL }, (char *const[]){ NULL },
                                        &kernel_proc);
    ktest_assert(srv != NULL, "cannot start the compositor");
    ktest_wait_idle(1200);

    struct proc *win = proc_create_user("/bin/compbench", (char *const[]){ "compbench", "window", NULL },
                                        (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(win != NULL, "cannot start compbench window");
    uint64_t t0 = timer_ms();
    while (!window_at_home() && timer_ms() - t0 < 4000)
        sleep_ms(50);
    ktest_assert(window_at_home(), "the compbench window is not shown: %08x", pixel(WIN_X + 2, WIN_Y - 10));
    ktest_wait_idle(500);
    int cx = logical_w() / 2, cy = logical_h() / 2;
    scenario_drag(&cx, &cy);
    scenario_pointer(&cx, &cy);
    scenario_resize(&cx, &cy);
    alt_key(0x3e);
    int status = proc_reap(win);
    ktest_assert(status == 0, "compbench window status 0x%x", status);

    const char *scenarios[] = { "blink", "anim", "idle" };
    for (int i = 0; i < 3; i++)
        run_scenario(scenarios[i]);
    signal_send(srv, SIGTERM);
    status = proc_reap(srv);
    ktest_assert(status == 0, "compositor status 0x%x", status);
    kprintf("comp_bench: ok\n");
}
KTEST_DEFINE("comp_bench", test_comp_bench);
