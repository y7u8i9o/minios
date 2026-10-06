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
#include <lib/crc32.h>
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

/* The CRC-32 of the guest framebuffer as rows of red, green and blue
 * bytes, the layout of a PPM image. The qmp script of the case takes a
 * screendump after the line, and the post script compares the CRC-32 of
 * the image of the host: every composed pixel must have been flushed. */
static void print_screen_crc(void)
{
    uint32_t w = (uint32_t)fb_screen.width, h = (uint32_t)fb_screen.height;
    uint8_t *row = kmalloc((size_t)w * 3);
    ktest_assert(row != NULL, "alloc");
    uint32_t crc = 0;
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            uint32_t c = fb_read_rgb(&fb_screen, x, y);
            row[3 * x] = (uint8_t)(c >> 16);
            row[3 * x + 1] = (uint8_t)(c >> 8);
            row[3 * x + 2] = (uint8_t)c;
        }
        crc = crc32(crc, row, (size_t)w * 3);
    }
    kfree(row);
    kprintf("comp_bench: screen crc %08x\n", crc);
    sleep_ms(1500);
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
    print_screen_crc();
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
 * corner, alternately 60x40 pixels inwards and back. After the 20 drags
 * the window has its first size again. */
static void resize_drags(int *cx, int *cy, bool reset_stats)
{
    int x = WIN_X + WIN_W + 3, y = WIN_Y + WIN_H + 3;
    mouse_move_to(cx, cy, x, y, 0);
    ktest_wait_idle(200);
    if (reset_stats)
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
}

static void scenario_resize(int *cx, int *cy)
{
    resize_drags(cx, cy, true);
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

/* G6: X12 with one window does nothing for five seconds. compstat then
 * reports no frame, and only the pings of the window and the connection
 * of compstat wake X12. The expect file bounds the wakeups. */
static void test_comp_idle(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
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
    ktest_assert(window_at_home(), "the compbench window is not shown");
    sleep_ms(1000);
    run_shell("compstat -r");
    sleep_ms(5000);
    kprintf("comp_idle: five idle seconds\n");
    run_shell("compstat");
    alt_key(0x3e);
    int status = proc_reap(win);
    ktest_assert(status == 0, "compbench window status 0x%x", status);
    signal_send(srv, SIGTERM);
    status = proc_reap(srv);
    ktest_assert(status == 0, "compositor status 0x%x", status);
    kprintf("comp_idle: ok\n");
}
KTEST_DEFINE("comp_idle", test_comp_idle);

/* G8: a window has two buffers in its pool and no private surface. The
 * compbench window at 2560x1600@2 has a buffer of 1584x1204 device
 * pixels. Its resident size must be below the G1 baseline of 23 MiB
 * minus one buffer, with a margin of 1 MiB. After 20 resize drags the
 * window has its first size again. The contents, a rounded frame corner
 * and the shadow must then show the pixels of the first frame. The
 * expect file checks the pool bytes that compbench prints at the close
 * request. */
static void test_gui_memory(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    ktest_assert(fb_screen_scale == 2, "the case needs scale 2");
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
    ktest_assert(window_at_home(), "the compbench window is not shown");
    ktest_wait_idle(500);

    /* Contents, the top left frame corner, the bottom right frame
     * corner, and the shadow below the frame. */
    static const int points[4][2] = {
        { WIN_X + 400, WIN_Y + 300 },
        { WIN_X, WIN_Y - 30 },
        { WIN_X + WIN_W - 1, WIN_Y + WIN_H - 1 },
        { WIN_X + 300, WIN_Y + WIN_H + 2 },
    };
    uint32_t before[4];
    for (int i = 0; i < 4; i++)
        before[i] = pixel(points[i][0], points[i][1]);
    unsigned long ms = 0, rss_kb = 0;
    proc_table_find("compbench", -1, &ms, &rss_kb);
    kprintf("gui_memory: compbench has %lu KiB resident before the resizes\n", rss_kb);

    int cx = logical_w() / 2, cy = logical_h() / 2;
    resize_drags(&cx, &cy, false);
    /* The pointer leaves the frame, which then shows no hover state. */
    mouse_move_to(&cx, &cy, logical_w() - 20, logical_h() - 20, 0);
    ktest_wait_idle(500);
    static const char *what[4] = { "contents", "top left corner", "bottom right corner", "shadow" };
    for (int i = 0; i < 4; i++) {
        uint32_t now = pixel(points[i][0], points[i][1]);
        ktest_assert(now == before[i], "the %s shows %06x after the resizes, %06x before", what[i], now, before[i]);
    }
    proc_table_find("compbench", -1, &ms, &rss_kb);
    unsigned long buffer_kb = (unsigned long)(WIN_W + 32) * 2 * (WIN_H + 30 + 32) * 2 * 4 / 1024;
    unsigned long bound = 23 * 1024 - buffer_kb + 1024;
    kprintf("gui_memory: compbench has %lu KiB resident after the resizes, bound %lu KiB\n", rss_kb, bound);
    ktest_assert(rss_kb < bound, "compbench has %lu KiB resident, more than %lu KiB", rss_kb, bound);

    alt_key(0x3e);
    int status = proc_reap(win);
    ktest_assert(status == 0, "compbench window status 0x%x", status);
    signal_send(srv, SIGTERM);
    status = proc_reap(srv);
    ktest_assert(status == 0, "compositor status 0x%x", status);
    kprintf("gui_memory: ok\n");
}
KTEST_DEFINE("gui_memory", test_gui_memory);

/* The device cursor of the case comp_cursor, read through /dev/fb0. */
static struct fb_cursor_state *cursor_state(void)
{
    static struct fb_cursor_state *c;
    if (!c)
        c = kmalloc(sizeof *c);
    ktest_assert(c != NULL, "alloc");
    fb_cursor_get(c);
    return c;
}

/* A screenshot into path, with the pointer when pointer is set. */
static uint32_t screenshot_crc(const char *path, bool pointer)
{
    vfs_unlink(path);
    char *const with[] = { "screenshot", "-p", (char *)path, NULL };
    char *const without[] = { "screenshot", (char *)path, NULL };
    struct proc *p = proc_create_user("/bin/screenshot", pointer ? with : without, (char *const[]){ NULL },
                                      &kernel_proc);
    ktest_assert(p != NULL, "cannot start screenshot");
    int status = proc_reap(p);
    ktest_assert(status == 0, "screenshot status 0x%x", status);
    long size;
    uint32_t crc = file_crc32(path, &size);
    ktest_assert(size > 0, "%s is empty", path);
    return crc;
}

/* G9: on virtio-gpu the device shows the cursor. Pointer motion moves the
 * device cursor and composes nothing. A cursor surface of a client
 * replaces the arrow on the device. A screenshot with the pointer
 * contains it, composed in software, and one without it differs. The
 * cursor disappears when X12 releases the display. */
static void test_comp_cursor(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    int32_t S = fb_screen_scale ? (int32_t)fb_screen_scale : 1;
    struct proc *srv = proc_create_user("/bin/x12", (char *const[]){ "x12", "-s", NULL }, (char *const[]){ NULL },
                                        &kernel_proc);
    ktest_assert(srv != NULL, "cannot start the compositor");
    ktest_wait_idle(1200);
    struct fb_cursor_state *c = cursor_state();
    ktest_assert(c->visible && c->width == (uint32_t)(13 * S) && c->height == (uint32_t)(19 * S),
                 "the device shows no arrow: visible %d, %ux%u", c->visible, c->width, c->height);
    int cx = logical_w() / 2, cy = logical_h() / 2;
    ktest_assert(arrow_cursor_at(cx, cy), "the arrow is not at the centre");

    run_shell("compstat -r");
    uint32_t moves = c->moves;
    int x = 0, y = 0;
    for (int i = 0; i < 400; i++) {
        x = 100 + (i % 20) * 30;
        y = 100 + (i % 13) * 25;
        mouse_move_to(&cx, &cy, x, y, 0);
    }
    ktest_wait_idle(300);
    kprintf("comp_cursor: pointer done\n");
    run_shell("compstat");
    c = cursor_state();
    kprintf("comp_cursor: %u device cursor moves\n", c->moves - moves);
    ktest_assert(c->moves - moves >= 20, "only %u device cursor moves", c->moves - moves);
    ktest_assert(arrow_cursor_at(x, y), "the arrow is not at %d,%d after the motion", x, y);

    /* The first toplevel appears at (40,60) with 200x150 pixels. */
    struct proc *cl = proc_create_user("/bin/comptest", (char *const[]){ "comptest", "cursor", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start comptest");
    ktest_wait_idle(1000);
    mouse_move_to(&cx, &cy, WIN_X + 100, WIN_Y + 75, 0);
    bool shown = false;
    for (int i = 0; i < 100 && !shown; i++) {
        sleep_ms(30);
        c = cursor_state();
        shown = c->visible && c->width == (uint32_t)(16 * S) && c->hot_x == (uint32_t)(8 * S) &&
                c->hot_y == (uint32_t)(8 * S) && c->image[0] == 0xffff0000u && c->x == (WIN_X + 100) * S;
    }
    ktest_assert(shown, "the device does not show the cursor surface: %ux%u hotspot %u,%u pixel %08x at %d,%d",
                 c->width, c->height, c->hot_x, c->hot_y, c->image[0], c->x, c->y);
    kprintf("comp_cursor: the cursor surface is on the device\n");

    /* The screenshot with the pointer differs only by the composed
     * cursor. */
    ktest_wait_idle(300);
    uint32_t with = screenshot_crc("/cursor-with.png", true);
    uint32_t without = screenshot_crc("/cursor-without.png", false);
    ktest_assert(with != without, "the screenshot with the pointer equals the one without");
    kprintf("comp_cursor: screenshots %08x with and %08x without the pointer\n", with, without);
    vfs_unlink("/cursor-with.png");
    vfs_unlink("/cursor-without.png");

    alt_key(0x3e);
    int status = proc_reap(cl);
    ktest_assert(status == 0, "comptest status 0x%x", status);
    ktest_wait_idle(300);
    c = cursor_state();
    ktest_assert(c->visible && c->width == (uint32_t)(13 * S), "the arrow did not return: %ux%u", c->width,
                 c->height);
    kprintf("comp_cursor: the arrow returned\n");

    signal_send(srv, SIGTERM);
    status = proc_reap(srv);
    ktest_assert(status == 0, "compositor status 0x%x", status);
    ktest_assert(!cursor_state()->visible, "the cursor remains after the release of the display");
    kprintf("comp_cursor: ok\n");
}
KTEST_DEFINE("comp_cursor", test_comp_cursor);
