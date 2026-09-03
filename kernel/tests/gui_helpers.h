#pragma once
/* Helpers shared by the GUI boot tests (test_gui.c, test_audio_gui.c):
 * synthetic mouse and keyboard input, logical pixels of the composed
 * screen and the desktop session processes. */
#include <tests/ktest.h>
#include <drivers/ps2mouse.h>
#include <drivers/ps2kbd.h>
#include <drivers/fbdev.h>
#include <drivers/timer.h>
#include <sched/user.h>
#include <sched/proc.h>
#include <ipc/signal.h>

static struct proc *panel_proc;

/* M17 stage 1: mouse packets become events on /dev/mouse. */
static inline void feed_packet_wheel(uint8_t flags, int dx, int dy, int dz)
{
    ps2mouse_feed_byte((uint8_t)(0x08 | flags | (dx < 0 ? 0x10 : 0) | (dy < 0 ? 0x20 : 0)));
    ps2mouse_feed_byte((uint8_t)dx);
    ps2mouse_feed_byte((uint8_t)dy);
    if (ps2mouse_has_wheel())
        ps2mouse_feed_byte((uint8_t)(dz & 0x0f));
}

static inline void feed_packet(uint8_t flags, int dx, int dy)
{
    feed_packet_wheel(flags, dx, dy, 0);
}

static inline void mouse_move_to(int *cx, int *cy, int x, int y, int held)
{
    while (*cx != x || *cy != y) {
        int dx = x - *cx, dy = y - *cy;
        if (dx > 100) dx = 100;
        if (dx < -100) dx = -100;
        if (dy > 100) dy = 100;
        if (dy < -100) dy = -100;
        /* The driver flips dy: positive packet dy means up. */
        feed_packet((uint8_t)held, dx, -dy);
        *cx += dx;
        *cy += dy;
    }
}

static inline void mouse_click(int buttons)
{
    feed_packet((uint8_t)buttons, 0, 0);
    feed_packet(0, 0, 0);
    sleep_ms(100);
}

/* A logical pixel of the composed screen: the GUI tests describe the
 * desktop in logical coordinates and run at any pixel scale. */
static inline uint32_t pixel(int x, int y)
{
    uint32_t s = fb_screen_scale ? fb_screen_scale : 1;
    return fb_read_rgb(&fb_screen, (uint32_t)x * s, (uint32_t)y * s);
}

/* The desktop's logical size. */
static inline int logical_w(void)
{
    return (int)(fb_screen.width / (fb_screen_scale ? fb_screen_scale : 1));
}

static inline int logical_h(void)
{
    return (int)(fb_screen.height / (fb_screen_scale ? fb_screen_scale : 1));
}

static inline uint32_t device_pixel(int x, int y)
{
    return fb_read_rgb(&fb_screen, (uint32_t)x, (uint32_t)y);
}

static inline void press_key(uint8_t code)
{
    ps2kbd_feed_scancode(code);
    ps2kbd_feed_scancode((uint8_t)(code | 0x80));
}

static inline void alt_key(uint8_t code)
{
    ps2kbd_feed_scancode(0x38);
    press_key(code);
    ps2kbd_feed_scancode(0xb8);
    sleep_ms(150);
}

/* The compositor and the panel; returns the compositor. */
static inline struct proc *start_server(void)
{
    struct proc *srv = proc_create_user("/bin/x12", (char *const[]){ "x12", NULL },
                                        (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(srv != NULL, "cannot start the compositor");
    sleep_ms(600);
    panel_proc = proc_create_user("/bin/panel", (char *const[]){ "panel", NULL }, (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(panel_proc != NULL, "cannot start the panel");
    sleep_ms(600);
    return srv;
}

static inline void stop_server(struct proc *srv)
{
    if (panel_proc) {
        signal_send(panel_proc, SIGTERM);
        proc_reap(panel_proc);
        panel_proc = NULL;
    }
    signal_send(srv, SIGTERM);
    int status = proc_reap(srv);
    ktest_assert(status == 0, "compositor status 0x%x", status);
}

/* M22: the framework test client. Window 400x300 at (40,60): menu bar
 * 6..32, text field 38..64, check box 70..96, then a row with the
 * editor (x 12..242), a scroll bar (248..262) and a list (268..388). */
static inline void ctrl_key(uint8_t code)
{
    ps2kbd_feed_scancode(0x1d);
    press_key(code);
    ps2kbd_feed_scancode(0x9d);
    sleep_ms(150);
}
