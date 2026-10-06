#include <tests/ktest.h>
#include <drivers/ps2mouse.h>
#include <drivers/ps2kbd.h>
#include <input/input.h>
#include <drivers/tty.h>
#include <sched/thread.h>
#include <sched/wait.h>
#include <drivers/fbdev.h>
#include <sched/user.h>
#include <sched/proc.h>
#include <boot.h>
#include <fs/vfs.h>
#include <lib/string.h>
#include <lib/date.h>
#include <drivers/rtc.h>
#include <lib/cmdline.h>
#include <lib/crc32.h>
#include <console.h>
#include <klog.h>
#include <errno.h>
#include <drivers/timer.h>
#include <arch/smp.h>
#include <cpu.h>
#include "gui_helpers.h"


/* M17 stage 1: a user program maps /dev/fb0 and draws a pattern that the
 * kernel verifies pixel by pixel; the console is handed over and back. */
static void test_fb0(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    vfs_unlink("/fb.ready");
    struct proc *p = proc_create_user("/bin/fbtest", (char *const[]){ "fbtest", NULL },
                                      (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(p != NULL, "cannot start /bin/fbtest");
    /* Wait until the program has drawn and is using the display. */
    struct inode *marker = NULL;
    for (int i = 0; i < 200 && vfs_lookup("/fb.ready", &marker) < 0; i++)
        sleep_ms(50);
    ktest_assert(marker != NULL, "fbtest did not signal readiness");
    inode_put(marker);
    const struct limine_framebuffer *lfb = &fb_screen;
    kprintf("fb0: %u bpp, r%u@%u g%u@%u b%u@%u\n", lfb->bpp, lfb->red_mask_size, lfb->red_mask_shift,
            lfb->green_mask_size, lfb->green_mask_shift, lfb->blue_mask_size, lfb->blue_mask_shift);
    uint32_t crc = 0;
    for (uint32_t y = 0; y < 64; y++) {
        for (uint32_t x = 0; x < 64; x++) {
            uint32_t want = ((x ^ y) & 1) ? 0x00ff8800 : 0x000044ff;
            uint32_t got = fb_read_rgb(lfb, x + 100, y + 100);
            ktest_assert(got == want, "pixel (%u,%u) = %08x, expected %08x", x, y, got, want);
            crc = crc32(crc, &got, sizeof got);
        }
    }
    kprintf("fb0: pattern verified, crc %08x\n", crc);
    /* Let the program release the display and exit. */
    ps2kbd_feed_scancode(0x1c);
    ps2kbd_feed_scancode(0x9c);
    int status = proc_reap(p);
    ktest_assert(status == 0, "fbtest status 0x%x", status);
    vfs_unlink("/fb.ready");
    /* The text console draws again: its glyph replaced the pattern. */
    kprintf("fb0: console restored\n");
}
KTEST_DEFINE("fb0", test_fb0);

/* M17 stage 3: the window server with a test client. Mouse packets and
 * scancodes are injected through the drivers; the server and the client
 * log what they see, and pixels of the composed screen are checked. */
#include <ipc/mqueue.h>
#include <ipc/signal.h>







static struct proc *start_server(void);
static void stop_server(struct proc *srv);

static void test_gui(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    struct proc *srv = start_server();
    ktest_wait_idle(200);
    ktest_assert(pixel(0, 0) == 0x00306080, "desktop pixel %08x", pixel(0, 0));

    struct proc *cl = proc_create_user("/bin/guitest", (char *const[]){ "guitest", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start guitest");
    ktest_wait_idle(800);
    /* Window 1 "alpha" 300x200 at (40,60), window 2 "beta" 240x160 at
     * (70,90); beta was created last and is focused. */
    ktest_assert(pixel(70 + 50, 90 + 40) == 0x00ff0000, "beta red rect pixel %08x", pixel(120, 130));
    ktest_assert(pixel(70 + 2, 90 - 10) == 0x00ebebeb, "beta title bar active %08x", pixel(72, 80));
    ktest_assert(pixel(40 + 2, 60 - 10) == 0x00fafafa, "alpha title bar inactive %08x", pixel(42, 50));
    ktest_assert(pixel(40 + 5, 60 + 100) == 0x00dcdcdc, "alpha contents %08x", pixel(45, 160));

    int cx = logical_w() / 2, cy = logical_h() / 2;
    /* Click inside beta, then type a key. */
    mouse_move_to(&cx, &cy, 100, 120, 0);
    mouse_click(1);
    ps2kbd_feed_scancode(0x1e);
    ps2kbd_feed_scancode(0x9e);
    ktest_wait_idle(100);
    /* Click alpha's contents left of beta (whose frame and its resize
     * border start at x 60): it comes to the front and gets focus. */
    mouse_move_to(&cx, &cy, 50, 200, 0);
    mouse_click(1);
    ktest_wait_idle(100);
    ktest_assert(pixel(40 + 2, 60 - 10) == 0x00ebebeb, "alpha title bar active after click %08x", pixel(42, 50));
    ktest_assert(pixel(70 + 50, 90 + 40) == 0x00dcdcdc, "alpha now covers beta: %08x", pixel(120, 130));
    /* Drag alpha by its title bar 100 pixels to the right. */
    mouse_move_to(&cx, &cy, 150, 50, 0);
    feed_packet(1, 0, 0);
    ktest_wait_idle(50);
    mouse_move_to(&cx, &cy, 250, 50, 1);
    feed_packet(0, 0, 0);
    ktest_wait_idle(200);
    ktest_assert(pixel(140 + 5, 60 + 100) == 0x00dcdcdc, "alpha moved: %08x", pixel(145, 160));
    ktest_assert(pixel(40 + 5, 60 + 100) == 0x00306080 || pixel(40 + 5, 60 + 100) == 0x00c8f0c8,
                 "old alpha area repainted: %08x", pixel(45, 160));
    /* Close alpha (the close button at the right of its header bar,
     * 19 px from the edge, 16 px above the contents), then beta. */
    mouse_move_to(&cx, &cy, 140 + 300 - 19, 60 - 16, 0);
    mouse_click(1);
    ktest_wait_idle(200);
    mouse_move_to(&cx, &cy, 70 + 240 - 19, 90 - 16, 0);
    mouse_click(1);
    int status = proc_reap(cl);
    ktest_assert(status == 0, "guitest status 0x%x", status);
    ktest_wait_idle(200);
    ktest_assert(pixel(70 + 50, 90 + 40) == 0x00306080, "desktop after close: %08x", pixel(120, 130));
    uint32_t crc = 0;
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            uint32_t v = pixel(x, y);
            crc = crc32(crc, &v, sizeof v);
        }
    kprintf("gui: desktop corner crc %08x\n", crc);
    signal_send(srv, SIGTERM);
    status = proc_reap(srv);
    ktest_assert(status == 0, "compositor status 0x%x", status);
    ktest_assert(tty_get_lflag(&console_tty) == (ICANON | ECHO | ISIG), "keyboard mode not restored");
    kprintf("gui: server stopped, console restored\n");
}
KTEST_DEFINE("gui", test_gui);

/* M17 stage 5: the terminal window runs the shell on a pseudo terminal.
 * Keys typed through the keyboard driver reach the shell, whose output
 * is proven by a file it writes; the window contents are checked for a
 * drawn glyph. */
static void test_gui_term(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    vfs_unlink("/gterm.txt");
    struct proc *srv = start_server();
    struct proc *term = proc_create_user("/bin/term", (char *const[]){ "term", NULL },
                                         (char *const[]){ "PATH=/bin", NULL }, &kernel_proc);
    ktest_assert(term != NULL, "cannot start term");
    sleep_ms(1500);
    /* Window 1 at (40,60), 662x431 (80x25 cells of 8x17 plus 4+4 and
     * 3+3 pixels of padding and the 14 pixel scrollback bar), dark
     * background with a prompt. */
    ktest_assert(pixel(40 + 300, 60 + 200) == 0x001e1e1e, "terminal background %08x", pixel(340, 260));
    type_line("echo typed into the terminal > /gterm.txt\n");
    type_line("cat /gterm.txt\n");
    struct inode *marker = NULL;
    for (int i = 0; i < 100 && vfs_lookup("/gterm.txt", &marker) < 0; i++)
        sleep_ms(50);
    ktest_assert(marker != NULL, "shell in the terminal did not write the file");
    inode_put(marker);
    sleep_ms(500);
    struct file *f;
    ktest_assert(vfs_open("/gterm.txt", O_RDONLY, 0, &f) == 0, "open /gterm.txt");
    char buf[64];
    long n = file_read(f, buf, sizeof buf - 1);
    file_put(f);
    buf[n > 0 ? n : 0] = '\0';
    ktest_assert(strcmp(buf, "typed into the terminal\n") == 0, "file content '%s'", buf);
    /* The prompt row was drawn: the antialiased text has some pixel of
     * the first text row brighter than half. */
    bool drawn = false;
    for (int y = 3; y < 20 && !drawn; y++)
        for (int x = 4; x < 640 && !drawn; x++)
            if ((pixel(40 + x, 60 + y) & 0xff) >= 0xa0)
                drawn = true;
    ktest_assert(drawn, "no text rendered in the terminal");
    /* M19: drag the grip so the window shrinks by 240x96 pixels (30
     * columns and 6 rows of the 8x17 cells: 422 - 8 - 14 = 400 pixels
     * fit 50 columns, 335 - 6 = 329 fit 19 rows); the shell sees the
     * size. */
    int sw = logical_w();
    int wx = 40, wy = 60;                       /* 662x431 at the cascade origin */
    ktest_assert(sw == 1024, "test assumes 1024 pixels of width");
    int cx = sw / 2, cy = logical_h() / 2;
    mouse_move_to(&cx, &cy, wx + 662 + 3, wy + 431 + 3, 0);
    feed_packet(1, 0, 0);
    sleep_ms(50);
    mouse_move_to(&cx, &cy, wx + 422 + 3, wy + 335 + 3, 1);
    feed_packet(0, 0, 0);
    sleep_ms(500);
    vfs_unlink("/gterm2.txt");
    type_line("stty size > /gterm2.txt\n");
    marker = NULL;
    for (int i = 0; i < 100 && vfs_lookup("/gterm2.txt", &marker) < 0; i++)
        sleep_ms(50);
    ktest_assert(marker != NULL, "stty did not write the file");
    inode_put(marker);
    sleep_ms(300);
    ktest_assert(vfs_open("/gterm2.txt", O_RDONLY, 0, &f) == 0, "open /gterm2.txt");
    n = file_read(f, buf, sizeof buf - 1);
    file_put(f);
    buf[n > 0 ? n : 0] = '\0';
    ktest_assert(strcmp(buf, "19 50\n") == 0, "window size seen by the shell '%s'", buf);
    /* Scrollback: 60 lines of output, then Shift+PageUp. */
    type_line("seq 1 60\n");
    sleep_ms(800);
    ps2kbd_feed_scancode(0x2a);
    ps2kbd_feed_scancode(0xe0);
    ps2kbd_feed_scancode(0x49);
    ps2kbd_feed_scancode(0xe0);
    ps2kbd_feed_scancode(0xc9);
    ps2kbd_feed_scancode(0xaa);
    sleep_ms(300);
    type_line("exit\n");
    int status = proc_reap(term);
    ktest_assert(status == 0, "term status 0x%x", status);
    signal_send(srv, SIGTERM);
    status = proc_reap(srv);
    ktest_assert(status == 0, "compositor status 0x%x", status);
    vfs_unlink("/gterm.txt");
    vfs_unlink("/gterm2.txt");
    kprintf("gui_term: shell output reached the file and the window\n");
}
KTEST_DEFINE("gui_term", test_gui_term);

/* ---- M19 stage 2 ---- */

/* Geometry of the launcher popup (user/panel/launcher.c) without
 * installed packages.  The menu has 6 px of padding, a 34 px search row, a
 * 22 px System heading and one 24 px row per entry of user/etc/launcher.
 * Log out is in the power menu of the panel since B4 of
 * docs/plan/desktop-panel.md.  It opens above the 28 px panel.  These
 * values must change with that file. */
#define LAUNCHER_SYSTEM  12
#define LAUNCHER_CLOCK   2     /* Clock=/bin/clock is the third System entry. */
#define LAUNCHER_H       (6 + 34 + 22 + LAUNCHER_SYSTEM * 24 + 6)
#define LAUNCHER_TOP(sh) ((sh) - 28 + 4 - LAUNCHER_H)
#define LAUNCHER_ROW(sh, i) (LAUNCHER_TOP(sh) + 6 + 34 + 22 + (i) * 24 + 12)





/* Resize by dragging the grip, maximize, restore and close a window;
 * the client refills the window green after every resize. */
static void test_gui_resize(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    int sw = logical_w(), sh = logical_h();
    int dh = sh - 28;                          /* desktop above the task bar */
    struct proc *srv = start_server();
    struct proc *cl = proc_create_user("/bin/guitest", (char *const[]){ "guitest", "resize", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start guitest");
    ktest_wait_idle(800);
    /* Window 1 "alpha" 300x200 at (40,60). Drag its bottom right
     * resize border (in the shadow outside the frame) by (100,50). */
    int cx = sw / 2, cy = sh / 2;
    mouse_move_to(&cx, &cy, 40 + 300 + 3, 60 + 200 + 3, 0);
    feed_packet(1, 0, 0);
    ktest_wait_idle(50);
    mouse_move_to(&cx, &cy, 40 + 400 + 3, 60 + 250 + 3, 1);
    feed_packet(0, 0, 0);
    ktest_wait_idle(500);
    ktest_assert(pixel(40 + 350, 60 + 220) == 0x0040c040, "resized contents %08x", pixel(390, 280));
    ktest_assert(pixel(40 + 2, 60 - 10) == 0x00ebebeb, "title bar after resize %08x", pixel(42, 50));
    /* Maximize: second button from the right of the header bar (22 px
     * buttons 6 px apart, the close button 8 px from the right edge;
     * the bar is 30 px tall, its buttons centred 16 px above the
     * contents). */
    mouse_move_to(&cx, &cy, 40 + 400 - 47, 60 - 16, 0);
    mouse_click(1);
    ktest_wait_idle(500);
    ktest_assert(pixel(sw - 10, dh - 10) == 0x0040c040, "maximized contents %08x", pixel(sw - 10, dh - 10));
    ktest_assert(pixel(sw / 2, sh - 14) == 0x0023272c || pixel(sw / 2, sh - 14) == 0x002e343b,
                 "task bar visible %08x", pixel(sw / 2, sh - 14));
    /* Restore through the same button, now at the top right of the
     * screen (the maximized frame starts at the top left corner). */
    mouse_move_to(&cx, &cy, sw - 47, 14, 0);
    mouse_click(1);
    ktest_wait_idle(500);
    ktest_assert(pixel(sw - 10, dh - 10) == 0x00306080, "desktop after restore %08x", pixel(sw - 10, dh - 10));
    ktest_assert(pixel(40 + 350, 60 + 220) == 0x0040c040, "restored contents %08x", pixel(390, 280));
    /* Close button of the header bar. */
    mouse_move_to(&cx, &cy, 40 + 400 - 19, 60 - 16, 0);
    mouse_click(1);
    int status = proc_reap(cl);
    ktest_assert(status == 0, "guitest status 0x%x", status);
    ktest_wait_idle(200);
    ktest_assert(pixel(40 + 350, 60 + 220) == 0x00306080, "desktop after close %08x", pixel(390, 280));
    stop_server(srv);
    kprintf("gui_resize: resize, maximize, restore and close ok\n");
}
KTEST_DEFINE("gui_resize", test_gui_resize);

/* Wheel, Alt+Tab, minimize to the task bar and restore from it,
 * occlusion culling, the launcher menu and Alt+F4. */
static void test_gui_wm(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    int sw = logical_w(), sh = logical_h();
    struct proc *srv = start_server();
    struct proc *cl = proc_create_user("/bin/guitest", (char *const[]){ "guitest", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start guitest");
    ktest_wait_idle(800);
    /* alpha 300x200 at (40,60), beta 240x160 at (70,90) on top and focused. */
    int cx = sw / 2, cy = sh / 2;
    mouse_move_to(&cx, &cy, 100, 120, 0);
    feed_packet_wheel(0, 0, 0, 1);
    ktest_wait_idle(150);
    /* Alt+Tab brings alpha, the lowest window, to the top. */
    alt_key(0x0f);
    ktest_assert(pixel(40 + 2, 60 - 10) == 0x00ebebeb, "alpha active after alt-tab %08x", pixel(42, 50));
    ktest_assert(pixel(70 + 50, 90 + 40) == 0x00dcdcdc, "alpha above beta %08x", pixel(120, 130));
    /* Minimize alpha: the third button from the right of its header
     * bar (22 px buttons 6 px apart, close 8 px from the edge). */
    mouse_move_to(&cx, &cy, 40 + 300 - 75, 60 - 16, 0);
    mouse_click(1);
    ktest_wait_idle(300);
    ktest_assert(pixel(45, 160) == 0x00306080, "alpha hidden %08x", pixel(45, 160));
    ktest_assert(pixel(70 + 50, 90 + 40) == 0x00ff0000, "beta visible again %08x", pixel(120, 130));
    /* Its task bar button (the first one) restores it. */
    mouse_move_to(&cx, &cy, PANEL_TASKS_X + 40, PANEL_ROW(sh), 0);
    mouse_click(1);
    ktest_wait_idle(300);
    ktest_assert(pixel(45, 160) == 0x00dcdcdc, "alpha restored %08x", pixel(45, 160));
    /* Bring beta to the top and maximize it: alpha is fully covered. */
    alt_key(0x0f);
    mouse_move_to(&cx, &cy, 70 + 240 - 47, 90 - 16, 0);
    mouse_click(1);
    ktest_wait_idle(500);
    ktest_assert(pixel(45, 160) == 0x0040c040, "beta covers alpha %08x", pixel(45, 160));
    /* The launcher menu starts the clock; Alt+F4 closes it. */
    mouse_move_to(&cx, &cy, 30, sh - 14, 0);
    mouse_click(1);
    ktest_wait_idle(200);
    mouse_move_to(&cx, &cy, 40, LAUNCHER_ROW(sh, LAUNCHER_CLOCK), 0);
    mouse_click(1);
    ktest_wait_idle(1500);
    alt_key(0x3e);
    ktest_wait_idle(300);
    /* Alt+F4 closes beta, then alpha; the client exits. */
    alt_key(0x3e);
    ktest_wait_idle(300);
    alt_key(0x3e);
    int status = proc_reap(cl);
    ktest_assert(status == 0, "guitest status 0x%x", status);
    ktest_wait_idle(200);
    ktest_assert(pixel(45, 160) == 0x00306080, "desktop after closing %08x", pixel(45, 160));
    stop_server(srv);
    kprintf("gui_wm: window management ok\n");
}
KTEST_DEFINE("gui_wm", test_gui_wm);

/* Two clients exchange text through the clipboard. */
static void test_gui_clip(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    struct proc *srv = start_server();
    struct proc *c1 = proc_create_user("/bin/guitest", (char *const[]){ "guitest", "clipset", "hello clipboard", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(c1 != NULL, "cannot start guitest clipset");
    int status = proc_reap(c1);
    ktest_assert(status == 0, "clipset status 0x%x", status);
    struct proc *c2 = proc_create_user("/bin/guitest", (char *const[]){ "guitest", "clipget", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(c2 != NULL, "cannot start guitest clipget");
    status = proc_reap(c2);
    ktest_assert(status == 0, "clipget status 0x%x", status);
    stop_server(srv);
    kprintf("gui_clip: clipboard ok\n");
}
KTEST_DEFINE("gui_clip", test_gui_clip);


static void test_gui_widgets(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    int sw = logical_w(), sh = logical_h();
    struct proc *srv = start_server();
    struct proc *cl = proc_create_user("/bin/widgettest", (char *const[]){ "widgettest", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start widgettest");
    ktest_wait_idle(1000);
    int cx = sw / 2, cy = sh / 2;
    mouse_move_to(&cx, &cy, 40 + 100, 60 + 51, 0);
    mouse_click(1);
    press_key(0x1e);
    press_key(0x30);
    ktest_wait_idle(150);
    ctrl_key(0x1e);
    ctrl_key(0x2e);
    mouse_move_to(&cx, &cy, 40 + 60, 60 + 150, 0);
    mouse_click(1);
    ctrl_key(0x2f);
    press_key(0x1c);
    press_key(0x2d);
    ktest_wait_idle(150);
    mouse_move_to(&cx, &cy, 40 + 277, 60 + 200, 0);
    feed_packet_wheel(0, 0, 0, 1);
    ktest_wait_idle(150);
    mouse_move_to(&cx, &cy, 40 + 12, 60 + 83, 0);
    mouse_click(1);
    mouse_move_to(&cx, &cy, 40 + 300, 60 + 150, 0);
    mouse_click(1);
    ps2kbd_feed_scancode(0xe0);
    ps2kbd_feed_scancode(0x4f);
    ps2kbd_feed_scancode(0xe0);
    ps2kbd_feed_scancode(0xcf);
    ktest_wait_idle(150);
    mouse_move_to(&cx, &cy, 40 + 20, 60 + 19, 0);
    mouse_click(1);
    mouse_move_to(&cx, &cy, 40 + 30, 60 + 45, 0);
    mouse_click(1);
    mouse_move_to(&cx, &cy, 40 + 20, 60 + 19, 0);
    mouse_click(1);
    mouse_move_to(&cx, &cy, 40 + 30, 60 + 70, 0);
    mouse_click(1);
    int status = proc_reap(cl);
    ktest_assert(status == 0, "widgettest status 0x%x", status);
    stop_server(srv);
    kprintf("gui_widgets: toolkit ok\n");
}
KTEST_DEFINE("gui_widgets", test_gui_widgets);

/* M22: combo box popup, spinner, slider, tabs. Window 400x300 at (40,60):
 * combo 6..32, spinner 38..64, slider 70..96, tabs from 102. */
static void test_gui_controls(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    int sw = logical_w(), sh = logical_h();
    struct proc *srv = start_server();
    struct proc *cl = proc_create_user("/bin/widgettest", (char *const[]){ "widgettest", "controls", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start widgettest controls");
    ktest_wait_idle(1000);
    int cx = sw / 2, cy = sh / 2;
    mouse_move_to(&cx, &cy, 40 + 100, 60 + 19, 0);
    mouse_click(1);
    ktest_wait_idle(200);
    mouse_move_to(&cx, &cy, 40 + 50, 60 + 85, 0);
    mouse_click(1);
    mouse_move_to(&cx, &cy, 40 + 394 - 9, 60 + 45, 0);
    mouse_click(1);
    mouse_move_to(&cx, &cy, 40 + 6 + 189 + 5, 60 + 83, 0);
    mouse_click(1);
    mouse_move_to(&cx, &cy, 40 + 20, 60 + 115, 0);
    mouse_click(1);
    ps2kbd_feed_scancode(0xe0);
    ps2kbd_feed_scancode(0x4d);
    ps2kbd_feed_scancode(0xe0);
    ps2kbd_feed_scancode(0xcd);
    ktest_wait_idle(200);
    mouse_move_to(&cx, &cy, 40 + 100, 60 + 149, 0);
    mouse_click(1);
    int status = proc_reap(cl);
    ktest_assert(status == 0, "widgettest status 0x%x", status);
    stop_server(srv);
    kprintf("gui_controls: controls ok\n");
}
KTEST_DEFINE("gui_controls", test_gui_controls);

/* M22: gedit types C source, highlights the keyword, saves with Ctrl+S.
 * Ctrl+A then selects the text, Copy in the context menu of the editor
 * copies it, and Ctrl+V pastes it on a second line. */
static void test_gui_editor(void)
{
    install_app("gedit");
    ktest_assert(fb_screen_present, "no framebuffer");
    struct proc *srv = start_server();
    vfs_unlink("/gedit.c");
    struct proc *cl = proc_create_user("/usr/bin/gedit", (char *const[]){ "gedit", "/gedit.c", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start gedit");
    ktest_wait_idle(1500);
    type_line("int x;");
    ktest_wait_idle(400);
    int blue = 0;
    for (int y = 60 + 82; y < 60 + 101; y++)
        for (int x = 40 + 8; x < 40 + 120; x++) {
            uint32_t v = pixel(x, y);
            int r = v >> 16 & 0xff, g = v >> 8 & 0xff, b = v & 0xff;
            if (b > r + 40 && b > g + 20)
                blue++;
        }
    ktest_assert(blue > 10, "keyword pixels are blue: %d", blue);
    ctrl_key(0x1f);
    ktest_wait_idle(400);
    struct file *f;
    ktest_assert(vfs_open("/gedit.c", O_RDONLY, 0, &f) == 0, "open /gedit.c");
    char buf[32];
    long n = file_read(f, buf, sizeof buf - 1);
    file_put(f);
    buf[n > 0 ? n : 0] = '\0';
    ktest_assert(strcmp(buf, "int x;") == 0, "saved text '%s'", buf);
    ctrl_key(0x1e);                     /* Ctrl+A */
    int cx = logical_w() / 2, cy = logical_h() / 2;
    mouse_move_to(&cx, &cy, 40 + 30, 60 + 92, 0);
    mouse_click(2);
    ktest_wait_idle(400);
    /* The menu lists Undo, Redo, a separator, Cut and Copy. */
    for (int i = 0; i < 5; i++) {
        ps2kbd_feed_scancode(0xe0);
        ps2kbd_feed_scancode(0x50);
        ps2kbd_feed_scancode(0xe0);
        ps2kbd_feed_scancode(0xd0);
        sleep_ms(50);
    }
    type_line("\n");
    ktest_wait_idle(300);
    ps2kbd_feed_scancode(0xe0);         /* End */
    ps2kbd_feed_scancode(0x4f);
    ps2kbd_feed_scancode(0xe0);
    ps2kbd_feed_scancode(0xcf);
    type_line("\n");
    ctrl_key(0x2f);                     /* Ctrl+V */
    ktest_wait_idle(300);
    ctrl_key(0x1f);                     /* Ctrl+S */
    ktest_wait_idle(400);
    ktest_assert(vfs_open("/gedit.c", O_RDONLY, 0, &f) == 0, "open /gedit.c again");
    n = file_read(f, buf, sizeof buf - 1);
    file_put(f);
    buf[n > 0 ? n : 0] = '\0';
    ktest_assert(strcmp(buf, "int x;\nint x;") == 0, "text after copy and paste '%s'", buf);
    alt_key(0x3e);
    int status = proc_reap(cl);
    ktest_assert(status == 0, "gedit status 0x%x", status);
    vfs_unlink("/gedit.c");
    stop_server(srv);
    kprintf("gui_editor: editor ok\n");
}
KTEST_DEFINE("gui_editor", test_gui_editor);

/* M20: a window draws a string with an outline font. Antialiasing
 * leaves intermediate colours between the black text and the white
 * background; a CRC of the text rows is logged for reference. */
static void test_gui_ttf(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    struct proc *srv = start_server();
    struct proc *cl = proc_create_user("/bin/guitest", (char *const[]){ "guitest", "ttf", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start guitest ttf");
    sleep_ms(1500);
    /* Window 1 at (40,60), 400x200, white, text at (10,10) in 32 px. */
    int black = 0, white = 0, grey = 0;
    uint32_t crc = 0;
    for (int y = 0; y < 48; y++)
        for (int x = 0; x < 380; x++) {
            uint32_t v = pixel(40 + 10 + x, 60 + 10 + y);
            crc = crc32(crc, &v, sizeof v);
            if (v == 0x00000000) black++;
            else if (v == 0x00ffffff) white++;
            else {
                uint32_t r = v >> 16 & 0xff, g = v >> 8 & 0xff, b = v & 0xff;
                if (r == g && g == b)
                    grey++;
            }
        }
    kprintf("gui_ttf: %d black, %d grey, %d white pixels, crc %08x\n", black, grey, white, crc);
    ktest_assert(black > 200, "too few black pixels: %d", black);
    ktest_assert(grey > 200, "too few antialiased pixels: %d", grey);
    ktest_assert(white > 5000, "too few white pixels: %d", white);
    alt_key(0x3e);
    int status = proc_reap(cl);
    ktest_assert(status == 0, "guitest status 0x%x", status);
    stop_server(srv);
    kprintf("gui_ttf: outline text rendered\n");
}
KTEST_DEFINE("gui_ttf", test_gui_ttf);

/* M21: the application framework client. Window "two" 200x120 at (40,60),
 * window "one" 400x300 at (70,90) on top: button at 6,6 388x30, then
 * the text field. */
static void test_gui_app(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    int sw = logical_w(), sh = logical_h();
    struct proc *srv = start_server();
    struct proc *cl = proc_create_user("/bin/apptest", (char *const[]){ "apptest", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start apptest");
    ktest_wait_idle(1500);
    int cx = sw / 2, cy = sh / 2;
    mouse_move_to(&cx, &cy, 70 + 6 + 194, 90 + 6 + 15, 0);
    mouse_click(1);
    ktest_wait_idle(300);
    ktest_assert(pixel(70 + 6 + 100, 90 + 6 + 15) == 0x00d0d0d0, "hovered button colour %08x", pixel(176, 111));
    press_key(0x0f);                    /* Tab: focus moves to the field */
    ktest_wait_idle(150);
    press_key(0x23);                    /* h */
    press_key(0x17);                    /* i */
    ktest_wait_idle(300);
    /* The list: third entry. */
    mouse_move_to(&cx, &cy, 70 + 100, 90 + 6 + 30 + 6 + 26 + 6 + 1 + 2 * 21 + 10, 0);
    mouse_click(1);
    ktest_wait_idle(300);
    alt_key(0x3e);                      /* close "one" */
    ktest_wait_idle(300);
    alt_key(0x3e);                      /* close "two": the loop ends */
    int status = proc_reap(cl);
    ktest_assert(status == 0, "apptest status 0x%x", status);
    stop_server(srv);
    kprintf("gui_app: application framework ok\n");
}
KTEST_DEFINE("gui_app", test_gui_app);

/* The calculator starts in RPN mode, accepts direct keyboard input and
 * changes to algebraic input through its mode selector. */
static void test_gui_calc(void)
{
    install_app("calc");
    ktest_assert(fb_screen_present, "no framebuffer");
    int sw = logical_w(), sh = logical_h();
    struct proc *srv = start_server();
    struct proc *cl = proc_create_user("/usr/bin/calc", (char *const[]){ "calc", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start calc");
    ktest_wait_idle(1200);
    ktest_assert(pixel(42, 50) == 0x00ebebeb,
                 "calculator window has an active title bar: %08x", pixel(42, 50));

    press_key(0x04);                    /* 3 ENTER 4 + */
    press_key(0x1c);
    press_key(0x05);
    ps2kbd_feed_scancode(0x2a);
    press_key(0x0d);
    ps2kbd_feed_scancode(0xaa);
    ktest_wait_idle(300);

    int cx = sw / 2, cy = sh / 2;
    mouse_move_to(&cx, &cy, 150, 79, 0);  /* mode combo in the first row */
    feed_packet_wheel(0, 0, 0, 1);        /* RPN -> Algebraic */
    ktest_wait_idle(300);
    mouse_move_to(&cx, &cy, 300, 130, 0); /* restore canvas keyboard focus */
    mouse_click(1);
    press_key(0x03);                    /* 2 + 3 * 4 ENTER */
    ps2kbd_feed_scancode(0x2a);
    press_key(0x0d);
    ps2kbd_feed_scancode(0xaa);
    press_key(0x04);
    ps2kbd_feed_scancode(0x2a);
    press_key(0x09);
    ps2kbd_feed_scancode(0xaa);
    press_key(0x05);
    press_key(0x1c);
    ktest_wait_idle(300);

    alt_key(0x3e);
    int status = proc_reap(cl);
    ktest_assert(status == 0, "calc status 0x%x", status);
    stop_server(srv);
    kprintf("gui_calc: RPN and algebraic input ok\n");
}
KTEST_DEFINE("gui_calc", test_gui_calc);

/* The Mandelbrot plotter: a 640x480 window at (40,60) whose canvas lies
 * below the menu bar and the tool bar.  The point at the window centre is
 * inside the set and painted black, and the left edge (about -2.2) is
 * outside and coloured.  A wheel step zooms in, a drag with the right
 * button zooms into a rectangle, j shows the Julia set of the centre,
 * Ctrl+S saves the image as a PNG file and Escape ends the program. */
static void test_gui_mandel(void)
{
    install_app("mandel");
    ktest_assert(fb_screen_present, "no framebuffer");
    int sw = logical_w(), sh = logical_h();
    struct proc *srv = start_server();
    struct proc *cl = proc_create_user("/usr/bin/mandel", (char *const[]){ "mandel", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start mandel");
    int mid_x = 40 + 320, mid_y = 60 + 300, edge_x = 40 + 8;
    uint32_t mid = 1, edge = 0;
    for (int i = 0; i < 300; i++) {
        mid = pixel(mid_x, mid_y);
        edge = pixel(edge_x, mid_y);
        if (mid == 0 && edge != 0 && edge != 0x00306080)
            break;
        sleep_ms(100);
    }
    ktest_assert(mid == 0, "centre of the set not black: %08x", mid);
    ktest_assert(edge != 0 && edge != 0x00306080, "left edge not coloured: %08x", edge);
    int cx = sw / 2, cy = sh / 2;
    mouse_move_to(&cx, &cy, mid_x, mid_y, 0);
    feed_packet_wheel(0x00, 0, 0, -1);      /* wheel up: zoom in */
    ktest_wait_idle(1500);
    mouse_move_to(&cx, &cy, mid_x - 80, mid_y - 60, 0);
    feed_packet(2, 0, 0);
    ktest_wait_idle(50);
    mouse_move_to(&cx, &cy, mid_x + 80, mid_y + 60, 1);
    feed_packet(0, 0, 0);
    ktest_wait_idle(1500);
    press_key(0x24);                        /* j */
    ktest_wait_idle(1500);
    vfs_unlink("/root/mandel.png");
    ctrl_key(0x1f);                         /* Ctrl+S */
    ktest_wait_idle(800);
    type_line("\n");
    ktest_wait_idle(800);
    struct file *f;
    ktest_assert(vfs_open("/root/mandel.png", O_RDONLY, 0, &f) == 0, "open /root/mandel.png");
    char sig[8] = { 0 };
    file_read(f, sig, sizeof sig);
    file_put(f);
    ktest_assert((uint8_t)sig[0] == 0x89 && sig[1] == 'P' && sig[2] == 'N' && sig[3] == 'G', "PNG signature %02x %02x",
                 (uint8_t)sig[0], (uint8_t)sig[1]);
    vfs_unlink("/root/mandel.png");
    press_key(0x01);                        /* escape */
    int status = proc_reap(cl);
    ktest_assert(status == 0, "mandel status 0x%x", status);
    stop_server(srv);
    kprintf("gui_mandel: progressive plotter ok\n");
}
KTEST_DEFINE("gui_mandel", test_gui_mandel);

/* Drag performance: 200 motion packets with the button pressed on the
 * terminal window's title bar; the elapsed time is logged and the
 * server reports compositions slower than 20 ms. */
static void test_gui_drag(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    int sw = logical_w(), sh = logical_h();
    struct proc *srv = start_server();
    struct proc *cl = proc_create_user("/bin/term", (char *const[]){ "term", NULL }, (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start term");
    ktest_wait_idle(1500);
    int cx = sw / 2, cy = sh / 2;
    mouse_move_to(&cx, &cy, 40 + 300, 60 - 10, 0);
    feed_packet(1, 0, 0);
    ktest_wait_idle(100);
    uint64_t t0 = timer_ms();
    for (int i = 0; i < 100; i++) {
        feed_packet(1, 1, 0);
        sleep_ms(4);
    }
    for (int i = 0; i < 100; i++) {
        feed_packet(1, -1, 0);
        sleep_ms(4);
    }
    feed_packet(0, 0, 0);
    /* The server has caught up when the window is back in place. */
    int settled = 0;
    for (int i = 0; i < 200 && !settled; i++) {
        sleep_ms(10);
        settled = pixel(40 + 2, 60 - 10) == 0x00ebebeb;
    }
    uint64_t dt = timer_ms() - t0;
    kprintf("gui_drag: 200 packets in %lu ms, settled %d\n", (unsigned long)dt, settled);
    ktest_assert(settled, "window did not settle");
    alt_key(0x3e);
    ktest_wait_idle(300);
    proc_reap(cl);
    stop_server(srv);
    kprintf("gui_drag: drag ok\n");
}
KTEST_DEFINE("gui_drag", test_gui_drag);

/* M24: the compositor core with the protocol test client. Surface 1 is
 * placed at (40,60); the client shows red, then green with one blue
 * pixel at (10,10). */
static struct proc *start_compositor(void)
{
    return start_x12((char *const[]){ "x12", "-s", "-v", NULL });
}

static void test_comp_core(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    struct proc *srv = start_compositor();
    struct proc *cl = proc_create_user("/bin/comptest", (char *const[]){ "comptest", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start comptest");
    ktest_wait_idle(1500);
    ktest_assert(pixel(40 + 100, 60 + 75) == 0x0000ff00, "green buffer on screen: %08x", pixel(140, 135));
    ktest_assert(pixel(40 + 10, 60 + 10) == 0x000000ff, "partial damage repainted: %08x", pixel(50, 70));
    int status = proc_reap(cl);
    ktest_assert(status == 0, "comptest status 0x%x", status);
    ktest_wait_idle(200);
    ktest_assert(pixel(40 + 100, 60 + 75) == 0x00306080, "desktop after the surface was destroyed: %08x", pixel(140, 135));
    signal_send(srv, SIGTERM);
    status = proc_reap(srv);
    ktest_assert(status == 0, "compositor status 0x%x", status);
    kprintf("comp_core: compositor core ok\n");
}
KTEST_DEFINE("comp_core", test_comp_core);

/* G5 of docs/plan/compositor-performance.md: comptest checks through the
 * frame statistics that X12 composes only the damage of a commit and
 * that commits with only a frame callback compose nothing. The case runs
 * at 2560x1600@2 on virtio-gpu, where X12 composes into the framebuffer. */
static void test_comp_damage(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    struct proc *srv = start_compositor();
    struct proc *cl = proc_create_user("/bin/comptest", (char *const[]){ "comptest", "damage", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start comptest");
    int status = proc_reap(cl);
    ktest_assert(status == 0, "comptest damage status 0x%x", status);
    signal_send(srv, SIGTERM);
    status = proc_reap(srv);
    ktest_assert(status == 0, "compositor status 0x%x", status);
    kprintf("comp_damage: ok\n");
}
KTEST_DEFINE("comp_damage", test_comp_damage);

/* video=WxH@2: the compositor composes at half the framebuffer size and
 * writes every logical pixel as a 2x2 block, so the comptest surface
 * appears at doubled coordinates and neighbouring pixels are equal. */
static void test_comp_scale(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    ktest_assert(bootinfo.fb_scale == 2, "fb scale %u, expected 2 (video=WxH@2)", bootinfo.fb_scale);
    struct proc *srv = start_compositor();
    struct proc *cl = proc_create_user("/bin/comptest", (char *const[]){ "comptest", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start comptest");
    ktest_wait_idle(1500);
    ktest_assert(device_pixel(2 * (40 + 100), 2 * (60 + 75)) == 0x0000ff00, "green buffer at doubled position: %08x",
                 device_pixel(280, 270));
    /* Logical (250,135) is right of the 200 wide surface at 40. */
    ktest_assert(device_pixel(2 * 250, 2 * 135) != 0x0000ff00, "right of the surface at doubled x: %08x", device_pixel(500, 270));
    /* Scan a row through the surface's left edge and the desktop. */
    for (int x = 30; x < 60; x++)
        for (int y = 130; y < 140; y++) {
            uint32_t v = device_pixel(2 * x, 2 * y);
            ktest_assert(device_pixel(2 * x + 1, 2 * y) == v && device_pixel(2 * x, 2 * y + 1) == v && device_pixel(2 * x + 1, 2 * y + 1) == v,
                         "pixel block at %d,%d not uniform", x, y);
        }
    ktest_assert(device_pixel(2 * 40, 2 * 135) == 0x0000ff00, "surface edge at doubled x: %08x", device_pixel(80, 270));
    ktest_assert(device_pixel(2 * 39, 2 * 135) != 0x0000ff00, "left of the surface at doubled x: %08x", device_pixel(78, 270));
    kprintf("comp_scale: pixels doubled\n");
    int status = proc_reap(cl);
    ktest_assert(status == 0, "comptest status 0x%x", status);
    signal_send(srv, SIGTERM);
    status = proc_reap(srv);
    ktest_assert(status == 0, "compositor status 0x%x", status);
}
KTEST_DEFINE("comp_scale", test_comp_scale);

/* A client killed without disconnecting must not block the server: its
 * queue fills, the server drops it after two seconds and continues to serve
 * a new client. */
static void test_gui_dead_client(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    int sw = logical_w(), sh = logical_h();
    struct proc *srv = start_server();
    struct proc *cl = proc_create_user("/bin/guitest", (char *const[]){ "guitest", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start guitest");
    ktest_wait_idle(800);
    signal_send(cl, SIGKILL);
    proc_reap(cl);
    /* Flood the focused window with more messages than the queue contains. */
    int cx = sw / 2, cy = sh / 2;
    mouse_move_to(&cx, &cy, 70 + 100, 90 + 80, 0);
    for (int i = 0; i < 40; i++) {
        mouse_click(1);
        press_key(0x1e);
    }
    ktest_wait_idle(2500);
    press_key(0x1e);
    ktest_wait_idle(300);
    struct proc *cl2 = proc_create_user("/bin/comptest", (char *const[]){ "comptest", "core", NULL },
                                        (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl2 != NULL, "cannot start the second client");
    int status = proc_reap(cl2);
    ktest_assert(status == 0, "the server still answers a new client: status 0x%x", status);
    stop_server(srv);
    kprintf("gui_dead_client: server survived a dead client\n");
}
KTEST_DEFINE("gui_dead_client", test_gui_dead_client);

/* M25: shell roles. Window "alpha" 200x150 gets a configure of 0x0
 * (the client picks the size), maps at (40,60), then the decorations
 * are used: a resize by the grip, maximize by its box, Alt+F4. */
static struct proc *start_client(const char *mode)
{
    struct proc *cl = proc_create_user("/bin/comptest", (char *const[]){ "comptest", (char *)mode, NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start comptest %s", mode);
    ktest_wait_idle(1000);
    return cl;
}

static void test_comp_shell(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    int sw = logical_w(), sh = logical_h();
    struct proc *srv = start_compositor();
    struct proc *cl = start_client("shell");
    /* Server decorations: the frame cascades to (40,30), so the 200x150
     * contents start at (41,59) under the 28 px title bar. */
    ktest_assert(pixel(40 + 100, 60 + 75) == 0x00dcdcdc, "window contents %08x", pixel(140, 135));
    ktest_assert(pixel(40 + 2, 59 - 12) == 0x00e9ecf0, "active title bar %08x", pixel(42, 47));
    int cx = sw / 2, cy = sh / 2;
    /* Grip drag: +100,+50. */
    mouse_move_to(&cx, &cy, 41 + 200 - 4, 59 + 150 - 4, 0);
    feed_packet(1, 0, 0);
    ktest_wait_idle(50);
    mouse_move_to(&cx, &cy, 41 + 300 - 4, 59 + 200 - 4, 1);
    feed_packet(0, 0, 0);
    ktest_wait_idle(500);
    ktest_assert(pixel(40 + 250, 60 + 175) == 0x00dcdcdc, "resized contents %08x", pixel(290, 235));
    /* Maximize button (second from the right, 14 px buttons 4 px
     * apart), then restore. */
    mouse_move_to(&cx, &cy, 41 + 300 - 27, 59 - 14, 0);
    mouse_click(1);
    ktest_wait_idle(500);
    ktest_assert(pixel(sw - 10, sh - 10) == 0x00dcdcdc, "maximized contents %08x", pixel(sw - 10, sh - 10));
    mouse_move_to(&cx, &cy, sw - 28, 29 - 14, 0);
    mouse_click(1);
    ktest_wait_idle(500);
    ktest_assert(pixel(sw - 10, sh - 10) == 0x00306080, "desktop after restore %08x", pixel(sw - 10, sh - 10));
    /* Move by the title bar: +60,+40. */
    mouse_move_to(&cx, &cy, 40 + 100, 59 - 12, 0);
    feed_packet(1, 0, 0);
    ktest_wait_idle(50);
    mouse_move_to(&cx, &cy, 40 + 160, 59 - 12 + 40, 1);
    feed_packet(0, 0, 0);
    ktest_wait_idle(300);
    ktest_assert(pixel(100 + 250, 100 + 175) == 0x00dcdcdc, "moved contents %08x", pixel(350, 275));
    alt_key(0x3e);
    int status = proc_reap(cl);
    ktest_assert(status == 0, "comptest status 0x%x", status);
    signal_send(srv, SIGTERM);
    proc_reap(srv);
    kprintf("comp_shell: shell ok\n");
}
KTEST_DEFINE("comp_shell", test_comp_shell);

/* M25: pointer and keyboard events with serials, keymap translation. */
static void test_comp_seat(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    int sw = logical_w(), sh = logical_h();
    struct proc *srv = start_compositor();
    struct proc *cl = start_client("seat");
    int cx = sw / 2, cy = sh / 2;
    /* The surface sits at (41,59) inside the server side frame. */
    mouse_move_to(&cx, &cy, 41 + 50, 59 + 40, 0);
    ktest_wait_idle(150);
    mouse_click(1);
    feed_packet_wheel(0, 0, 0, 1);
    ktest_wait_idle(150);
    press_key(0x1e);                    /* a */
    ps2kbd_feed_scancode(0x2a);
    press_key(0x1e);                    /* A */
    ps2kbd_feed_scancode(0xaa);
    ps2kbd_feed_scancode(0xe0);
    ps2kbd_feed_scancode(0x48);
    ps2kbd_feed_scancode(0xe0);
    ps2kbd_feed_scancode(0xc8);         /* up */
    ktest_wait_idle(300);
    alt_key(0x3e);
    int status = proc_reap(cl);
    ktest_assert(status == 0, "comptest status 0x%x", status);
    signal_send(srv, SIGTERM);
    proc_reap(srv);
    kprintf("comp_seat: seat ok\n");
}
KTEST_DEFINE("comp_seat", test_comp_seat);

/* A client that stops answering pings is dimmed and gets the not
 * responding dialog; Wait hides it, the client recovers; a client that
 * hangs for good is killed through Force quit. comptest windows are
 * 200x150, the dialog 184x96 centred with two 76x26 buttons. */
static void test_comp_hang(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    struct proc *srv = start_compositor();
    int cx = logical_w() / 2, cy = logical_h() / 2;
    struct proc *cl = proc_create_user("/bin/comptest", (char *const[]){ "comptest", "hang", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start comptest");
    /* Toplevel 1 at (40,60): dimmed 0xd0d0ff is 0x88889f. */
    uint64_t t0 = timer_ms();
    while (pixel(45, 65) != 0x0088889f && timer_ms() - t0 < 8000)
        sleep_ms(100);
    kprintf("comp_hang: dialog after %lu ms\n", timer_ms() - t0);
    ktest_assert(pixel(45, 65) == 0x0088889f, "hung window dimmed: %08x", pixel(45, 65));
    ktest_assert(pixel(56, 95) == 0x00f4f5f7, "dialog box drawn: %08x", pixel(56, 95));
    /* Wait: the dialog goes away although the client still hangs. */
    mouse_move_to(&cx, &cy, 60 + 38, 145 + 13, 0);
    mouse_click(1);
    sleep_ms(300);
    ktest_assert(pixel(45, 65) == 0x00d0d0ff, "window shown again after Wait: %08x", pixel(45, 65));
    int status = proc_reap(cl);
    ktest_assert(status == 0, "hanging client status 0x%x", status);

    /* Toplevel 2 at (70,90) hangs for good; Force quit kills it. */
    cl = proc_create_user("/bin/comptest", (char *const[]){ "comptest", "hang-forever", NULL },
                          (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start comptest");
    t0 = timer_ms();
    while (pixel(75, 95) != 0x0088889f && timer_ms() - t0 < 8000)
        sleep_ms(100);
    ktest_assert(pixel(75, 95) == 0x0088889f, "second hung window dimmed: %08x", pixel(75, 95));
    char pause_arg[8];
    if (cmdline_lookup("pause", pause_arg, sizeof pause_arg) && pause_arg[0] == '1')
        sleep_ms(8000);                 /* screenshots of the dialog */
    mouse_move_to(&cx, &cy, 174 + 38, 175 + 13, 0);
    mouse_click(1);
    status = proc_reap(cl);
    ktest_assert(status == PROC_STATUS_SIGNALED(SIGKILL), "force quit killed the client: 0x%x", status);
    sleep_ms(300);
    ktest_assert(pixel(75, 95) == 0x00306080, "window gone after Force quit: %08x", pixel(75, 95));
    signal_send(srv, SIGTERM);
    proc_reap(srv);
    kprintf("comp_hang: ok\n");
}
KTEST_DEFINE("comp_hang", test_comp_hang);

/* M25: the selection and a drag from the source window (surface 1 at
 * 40,60) to the target window (surface 2 at 70,90, on top). */
static void test_comp_data(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    int sw = logical_w(), sh = logical_h();
    struct proc *srv = start_compositor();
    struct proc *src = start_client("data-source");
    struct proc *dst = start_client("data-target");
    int cx = sw / 2, cy = sh / 2;
    /* Press in the source (its visible strip left of the target). */
    mouse_move_to(&cx, &cy, 40 + 10, 60 + 100, 0);
    ktest_wait_idle(100);
    feed_packet(1, 0, 0);
    ktest_wait_idle(400);
    mouse_move_to(&cx, &cy, 70 + 100, 90 + 75, 1);
    ktest_wait_idle(300);
    feed_packet(0, 0, 0);
    int status = proc_reap(dst);
    ktest_assert(status == 0, "target status 0x%x", status);
    status = proc_reap(src);
    ktest_assert(status == 0, "source status 0x%x", status);
    kprintf("comp_data: data device ok\n");
    /* The actions: the target prefers move, Ctrl forces copy while it is
     * down, and Escape cancels the drag, which the target learns through
     * leave and the source through cancelled. */
    src = start_client("drag-source");
    dst = start_client("drag-target");
    /* Toplevels 3 and 4 cascade to contents at (100,120) and (130,150). */
    mouse_move_to(&cx, &cy, 100 + 10, 120 + 100, 0);
    ktest_wait_idle(100);
    feed_packet(1, 0, 0);
    ktest_wait_idle(400);
    mouse_move_to(&cx, &cy, 130 + 100, 150 + 75, 1);
    ktest_wait_idle(300);
    ps2kbd_feed_scancode(0x1d);
    ktest_wait_idle(300);
    ps2kbd_feed_scancode(0x9d);
    ktest_wait_idle(300);
    press_key(0x01);
    ktest_wait_idle(300);
    feed_packet(0, 0, 0);
    status = proc_reap(dst);
    ktest_assert(status == 0, "drag target status 0x%x", status);
    status = proc_reap(src);
    ktest_assert(status == 0, "drag source status 0x%x", status);
    signal_send(srv, SIGTERM);
    proc_reap(srv);
    kprintf("comp_data: drag actions ok\n");
}
KTEST_DEFINE("comp_data", test_comp_data);

/* M25: the panel lists toplevels and launches the clock (which still
 * runs on wsrv until M26, so the launch itself is what is checked). */
static void test_comp_panel(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    int sw = logical_w(), sh = logical_h();
    struct proc *srv = start_compositor();
    struct proc *panel = proc_create_user("/bin/panel", (char *const[]){ "panel", NULL }, (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(panel != NULL, "cannot start the panel");
    ktest_wait_idle(1000);
    ktest_assert(pixel(sw / 2, sh - 14) == 0x0023272c, "panel drawn at the bottom: %08x", pixel(sw / 2, sh - 14));
    struct proc *cl = start_client("shell");
    ktest_assert(pixel(PANEL_TASKS_X + 4, PANEL_ROW(sh)) == PANEL_BUTTON_ACTIVE, "task button for the active window: %08x",
                 pixel(PANEL_TASKS_X + 4, PANEL_ROW(sh)));
    int cx = sw / 2, cy = sh / 2;
    /* Minimize through the task button, restore through it. */
    mouse_move_to(&cx, &cy, PANEL_TASKS_X + 40, PANEL_ROW(sh), 0);
    mouse_click(1);
    ktest_wait_idle(400);
    ktest_assert(pixel(40 + 100, 60 + 75) == 0x00306080, "window hidden after the task click: %08x", pixel(140, 135));
    /* The button of the minimized window is highlighted under the pointer. */
    ktest_assert(pixel(PANEL_TASKS_X + 4, PANEL_ROW(sh)) == PANEL_BUTTON_HOVER, "highlight under the pointer: %08x",
                 pixel(PANEL_TASKS_X + 4, PANEL_ROW(sh)));
    mouse_click(1);
    ktest_wait_idle(400);
    ktest_assert(pixel(40 + 100, 60 + 75) == 0x00dcdcdc, "window restored: %08x", pixel(140, 135));
    /* The button shows the icon of the application (app-default for the
     * app_id comptest) left of the title. */
    int light = 0;
    for (int y = sh - PANEL_H + 6; y < sh - 6; y++)
        for (int x = PANEL_TASKS_X + 8; x < PANEL_TASKS_X + 24; x++)
            if ((pixel(x, y) & 0xff) > 0xa0)
                light++;
    ktest_assert(light >= 20, "icon on the window button: %d light pixels", light);
    kprintf("comp_panel: icon of %d pixels on the window button\n", light);
    /* Two clients with two windows each give five buttons, which do not
     * fit at 160 pixels left of the input label at a width of 1024. Each
     * becomes narrower, and the fifth, active button begins at the fifth
     * position of the narrower width. */
    struct proc *more[2];
    for (int i = 0; i < 2; i++) {
        more[i] = proc_create_user("/bin/guitest", (char *const[]){ "guitest", NULL }, (char *const[]){ NULL },
                                   &kernel_proc);
        ktest_assert(more[i] != NULL, "cannot start guitest");
        ktest_wait_idle(1000);
    }
    int narrow = (PANEL_INPUT_X(sw) - 8 - PANEL_TASKS_X + 4) / 5 - 4;
    int fifth = PANEL_TASKS_X + 4 * (narrow + 4);
    ktest_assert(sw == 1024 && narrow < PANEL_TASK_W, "button width %d at a screen width of %d", narrow, sw);
    ktest_assert(pixel(fifth + 4, PANEL_ROW(sh)) == PANEL_BUTTON_ACTIVE &&
                     pixel(fifth + narrow - 4, PANEL_ROW(sh)) == PANEL_BUTTON_ACTIVE &&
                     pixel(fifth + narrow + 2, PANEL_ROW(sh)) != PANEL_BUTTON_ACTIVE,
                 "the fifth button at %d: %08x %08x %08x", fifth, pixel(fifth + 4, PANEL_ROW(sh)),
                 pixel(fifth + narrow - 4, PANEL_ROW(sh)), pixel(fifth + narrow + 2, PANEL_ROW(sh)));
    kprintf("comp_panel: five window buttons of %d pixels\n", narrow);
    /* The QMP script takes a screendump of the bar for inspection. */
    sleep_ms(1500);
    for (int i = 0; i < 2; i++) {
        signal_send(more[i], SIGTERM);
        proc_reap(more[i]);
    }
    ktest_wait_idle(600);
    /* The power menu opens above the power button with Lock, Log out,
     * Restart and Shut down, and a click outside it dismisses it. */
    mouse_move_to(&cx, &cy, PANEL_POWER_X(sw) + PANEL_POWER_W / 2, PANEL_ROW(sh), 0);
    mouse_click(1);
    ktest_wait_idle(600);
    int mx = PANEL_POWER_X(sw) + PANEL_POWER_W - POWER_MENU_W, my = sh - PANEL_H + 4 - POWER_MENU_H;
    ktest_assert(pixel(mx + 2, my + 2) == 0x00fafbfc, "power menu above its button: %08x", pixel(mx + 2, my + 2));
    mouse_move_to(&cx, &cy, mx + 60, my + 6 + 28 + 14, 0);
    ktest_wait_idle(400);
    kprintf("comp_panel: power menu shown\n");
    sleep_ms(1500);                     /* the screendump of the QMP script */
    mouse_move_to(&cx, &cy, sw - 100, 100, 0);
    mouse_click(1);
    ktest_wait_idle(400);
    ktest_assert(pixel(mx + 2, my + 2) != 0x00fafbfc, "power menu dismissed: %08x", pixel(mx + 2, my + 2));
    /* The launcher menu is opened and dismissed with a click on the
     * desktop, which makes the compositor send popup.done.  It is opened
     * again, and the search for "clo" and Enter start the clock.  The
     * reopening once asked for a popup role on a surface that still had
     * one, a protocol error that disconnected the panel. */
    mouse_move_to(&cx, &cy, 30, sh - 14, 0);
    mouse_click(1);
    ktest_wait_idle(400);
    mouse_move_to(&cx, &cy, sw - 100, 100, 0);
    mouse_click(1);
    ktest_wait_idle(400);
    ktest_assert(pixel(sw / 2, sh - 14) == 0x0023272c, "panel alive after the dismissal: %08x", pixel(sw / 2, sh - 14));
    mouse_move_to(&cx, &cy, 30, sh - 14, 0);
    mouse_click(1);
    ktest_wait_idle(400);
    type_line("clo\n");
    ktest_wait_idle(1200);
    alt_key(0x3e);                      /* closes the clock, which is on top */
    ktest_wait_idle(300);
    alt_key(0x3e);                      /* closes the test window */
    int status = proc_reap(cl);
    ktest_assert(status == 0, "comptest status 0x%x", status);
    signal_send(panel, SIGTERM);
    proc_reap(panel);
    signal_send(srv, SIGTERM);
    proc_reap(srv);
    kprintf("comp_panel: panel ok\n");
}
KTEST_DEFINE("comp_panel", test_comp_panel);

/* B3 of docs/plan/desktop-panel.md: a click on the clock opens the
 * calendar of the current month above the clock, with today marked in
 * the accent colour. The forward button shows the next month without the
 * mark, the back button the current month again, and a second click on
 * the clock closes it. The test image has no /etc/localtime, so the panel
 * shows the date in UTC, and the C locale begins the week on Sunday. */
#define CAL_W (2 * 6 + 7 * 36)
#define CAL_H (2 * 6 + 32 + 24 + 6 * 28)
#define CAL_ACCENT 0x005b9cf5
#define CAL_BG 0x00fafbfc

static void today_cell(int x0, int y0, int *cx, int *cy, int *year, int *month)
{
    int64_t days = (int64_t)(rtc_realtime_ns() / 1000000000ULL / 86400);
    int y, m, d;
    date_civil_from_days(days, &y, &m, &d);
    /* 1970-01-01 was a Thursday. */
    int first = (int)((date_days_from_civil(y, m, 1) + 4) % 7);
    int slot = first + d - 1;
    *cx = x0 + 6 + (slot % 7) * 36 + 6;
    *cy = y0 + 6 + 32 + 24 + (slot / 7) * 28 + 14;
    *year = y;
    *month = m;
}

static void test_panel_calendar(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    int sw = logical_w(), sh = logical_h();
    struct proc *srv = start_compositor();
    struct proc *panel = proc_create_user("/bin/panel", (char *const[]){ "panel", NULL }, (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(panel != NULL, "cannot start the panel");
    ktest_wait_idle(1000);
    int cx = sw / 2, cy = sh / 2;
    mouse_move_to(&cx, &cy, PANEL_CLOCK_X(sw) + PANEL_CLOCK_W / 2, PANEL_ROW(sh), 0);
    mouse_click(1);
    ktest_wait_idle(600);
    int x0 = PANEL_CLOCK_X(sw) + PANEL_CLOCK_W - 4 - CAL_W, y0 = sh - PANEL_H + 4 - CAL_H;
    int tx, ty, year, month;
    today_cell(x0, y0, &tx, &ty, &year, &month);
    ktest_assert(pixel(x0 + 2, y0 + 2) == CAL_BG, "calendar above the clock: %08x", pixel(x0 + 2, y0 + 2));
    ktest_assert(pixel(tx, ty) == CAL_ACCENT, "today marked at %d,%d: %08x", tx, ty, pixel(tx, ty));
    kprintf("panel_calendar: %04d-%02d shown, today marked\n", year, month);
    sleep_ms(1500);                     /* the screendump of the QMP script */
    /* The next month has no mark at the same place. */
    mouse_move_to(&cx, &cy, x0 + CAL_W - 6 - 16, y0 + 6 + 16, 0);
    mouse_click(1);
    ktest_wait_idle(400);
    ktest_assert(pixel(tx, ty) != CAL_ACCENT, "no mark in the next month: %08x", pixel(tx, ty));
    mouse_move_to(&cx, &cy, x0 + 6 + 16, y0 + 6 + 16, 0);
    mouse_click(1);
    ktest_wait_idle(400);
    ktest_assert(pixel(tx, ty) == CAL_ACCENT, "today marked again: %08x", pixel(tx, ty));
    /* A second click on the clock closes the calendar. */
    mouse_move_to(&cx, &cy, PANEL_CLOCK_X(sw) + PANEL_CLOCK_W / 2, PANEL_ROW(sh), 0);
    mouse_click(1);
    ktest_wait_idle(600);
    ktest_assert(pixel(x0 + 2, y0 + 2) == 0x00306080, "calendar closed: %08x", pixel(x0 + 2, y0 + 2));
    kprintf("panel_calendar: ok\n");
    signal_send(panel, SIGTERM);
    proc_reap(panel);
    signal_send(srv, SIGTERM);
    proc_reap(srv);
}
KTEST_DEFINE("panel_calendar", test_panel_calendar);

/* B5 of docs/plan/desktop-panel.md: the show desktop button at the right
 * edge minimizes the three windows of comptest and guitest, and a second
 * click restores them with beta, the active window, on top. A window that
 * opens while the desktop is shown ends the recorded state, so the next
 * click shows the desktop again and minimizes the new window, the only
 * visible one, instead of restoring the three. */
/* The background or the red rectangle of the window beta of guitest. */
static bool is_beta(uint32_t c)
{
    return c == 0x00c8f0c8 || c == 0x00ff0000;
}

static void click_show_desktop(int sw, int sh, int *cx, int *cy)
{
    mouse_move_to(cx, cy, PANEL_DESKTOP_X(sw) + PANEL_DESKTOP_W / 2, PANEL_ROW(sh), 0);
    mouse_click(1);
    ktest_wait_idle(800);
}

static void test_panel_desktop(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    int sw = logical_w(), sh = logical_h();
    struct proc *srv = start_compositor();
    struct proc *panel = proc_create_user("/bin/panel", (char *const[]){ "panel", NULL }, (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(panel != NULL, "cannot start the panel");
    ktest_wait_idle(1000);
    struct proc *shell = start_client("shell");
    struct proc *two = proc_create_user("/bin/guitest", (char *const[]){ "guitest", NULL }, (char *const[]){ NULL },
                                        &kernel_proc);
    ktest_assert(two != NULL, "cannot start guitest");
    ktest_wait_idle(1200);
    ktest_assert(is_beta(pixel(70 + 50, 90 + 40)), "beta on top: %08x", pixel(120, 130));
    int cx = sw / 2, cy = sh / 2;
    click_show_desktop(sw, sh, &cx, &cy);
    ktest_assert(pixel(70 + 50, 90 + 40) == 0x00306080 && pixel(40 + 100, 60 + 75) == 0x00306080,
                 "the desktop where the windows were: %08x %08x", pixel(120, 130), pixel(140, 135));
    kprintf("panel_desktop: desktop shown\n");
    click_show_desktop(sw, sh, &cx, &cy);
    ktest_assert(is_beta(pixel(70 + 50, 90 + 40)), "beta on top again: %08x", pixel(120, 130));
    kprintf("panel_desktop: windows restored\n");
    /* A window opened in between ends the recorded state. */
    click_show_desktop(sw, sh, &cx, &cy);
    struct proc *late = start_client("shell");
    ktest_wait_idle(800);
    click_show_desktop(sw, sh, &cx, &cy);
    kprintf("panel_desktop: ok\n");
    signal_send(late, SIGTERM);
    proc_reap(late);
    signal_send(two, SIGTERM);
    proc_reap(two);
    signal_send(shell, SIGTERM);
    proc_reap(shell);
    signal_send(panel, SIGTERM);
    proc_reap(panel);
    signal_send(srv, SIGTERM);
    proc_reap(srv);
}
KTEST_DEFINE("panel_desktop", test_panel_desktop);

/* B6 of docs/plan/desktop-panel.md: with panel_position=top the panel is
 * at the top edge and the desktop area begins below it. A window cascades
 * from (40, 30) of that area, so its frame is at (40, 58) and its 200x150
 * contents under the 28 pixel title bar of the server decorations begin at
 * (41, 87). Its maximized frame begins below the panel, and the launcher
 * opens downwards. The setting bottom moves the running panel to the
 * bottom edge, and the maximized window then begins at the top edge. */
static void set_panel_position(const char *value)
{
    struct proc *p = proc_create_user("/bin/settings", (char *const[]){ "settings", "set", "panel_position", (char *)value, NULL },
                                      (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(p != NULL && proc_reap(p) == 0, "settings set panel_position %s", value);
}

static void test_panel_top(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    int sw = logical_w(), sh = logical_h();
    struct proc *srv = start_compositor();
    set_panel_position("top");
    struct proc *panel = proc_create_user("/bin/panel", (char *const[]){ "panel", NULL }, (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(panel != NULL, "cannot start the panel");
    ktest_wait_idle(1000);
    ktest_assert(pixel(sw / 2, PANEL_H / 2) == PANEL_BG && pixel(sw / 2, sh - 14) == 0x00306080,
                 "panel at the top: %08x, desktop at the bottom: %08x", pixel(sw / 2, PANEL_H / 2), pixel(sw / 2, sh - 14));
    kprintf("panel_top: panel at the top\n");
    struct proc *cl = start_client("shell");
    ktest_assert(pixel(40 + 100, 88 + 75) == 0x00dcdcdc, "window below the panel: %08x", pixel(140, 163));
    int cx = sw / 2, cy = sh / 2;
    mouse_move_to(&cx, &cy, 41 + 200 - 27, 87 - 14, 0);
    mouse_click(1);
    ktest_wait_idle(600);
    ktest_assert(pixel(sw / 2, PANEL_H + 5) == 0x00e9ecf0 && pixel(sw - 10, sh - 10) == 0x00dcdcdc,
                 "maximized below the panel: title %08x, contents %08x", pixel(sw / 2, PANEL_H + 5),
                 pixel(sw - 10, sh - 10));
    ktest_assert(pixel(sw / 2, PANEL_H / 2) == PANEL_BG, "panel over the maximized window: %08x", pixel(sw / 2, 14));
    kprintf("panel_top: maximized window below the panel\n");
    /* The launcher opens below its button. */
    mouse_move_to(&cx, &cy, PANEL_MENU_X, PANEL_H / 2, 0);
    mouse_click(1);
    ktest_wait_idle(600);
    ktest_assert(pixel(8, PANEL_H + 4) == 0x00fafbfc, "launcher below its button: %08x", pixel(8, PANEL_H + 4));
    kprintf("panel_top: launcher below its button\n");
    sleep_ms(1500);                     /* the screendump of the QMP script */
    mouse_move_to(&cx, &cy, sw - 100, sh / 2, 0);
    mouse_click(1);
    ktest_wait_idle(400);
    /* The running panel moves to the bottom edge within a second. */
    set_panel_position("bottom");
    sleep_ms(1500);
    ktest_wait_idle(600);
    ktest_assert(pixel(sw / 2, sh - 14) == PANEL_BG && pixel(sw / 2, 5) == 0x00e9ecf0,
                 "panel at the bottom: %08x, maximized title at the top: %08x", pixel(sw / 2, sh - 14), pixel(sw / 2, 5));
    kprintf("panel_top: panel moved to the bottom\n");
    signal_send(cl, SIGTERM);
    proc_reap(cl);
    signal_send(panel, SIGTERM);
    proc_reap(panel);
    signal_send(srv, SIGTERM);
    proc_reap(srv);
}
KTEST_DEFINE("panel_top", test_panel_top);

/* A compositor that dies (here killed) must give the keyboard back to
 * the console: closing its descriptor drops the EVIOCGRAB grab. */
static void test_gui_kbd_restore(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    /* Pid 1 is treated as init and cannot be killed: use up that pid. */
    struct proc *first = proc_create_user("/bin/hello", (char *const[]){ "hello", NULL }, (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(first != NULL, "cannot start hello");
    proc_reap(first);
    /* X12 grabs the input devices before it creates its socket. */
    struct proc *srv = start_x12((char *const[]){ "x12", "-s", NULL });
    ktest_assert(input_dev_grabbed(ps2kbd_device()), "compositor did not grab the keyboard");
    ktest_assert(input_dev_grabbed(ps2mouse_device()), "compositor did not grab the mouse");
    signal_send(srv, SIGKILL);
    proc_reap(srv);
    sleep_ms(100);
    ktest_assert(!input_dev_grabbed(ps2kbd_device()), "keyboard still grabbed after the compositor died");
    ktest_assert(tty_get_lflag(&console_tty) == (ICANON | ECHO | ISIG), "console mode changed");
    kprintf("gui_kbd_restore: keyboard mode restored\n");
}
KTEST_DEFINE("gui_kbd_restore", test_gui_kbd_restore);

/* Debugging tools (after M26): the settings application applies a
 * setting without a window and reports statistics; evtest logs a
 * click; sysmon, logview and hexview open and close. */
static void test_gui_tools(void)
{
    install_app("hexview");
    ktest_assert(fb_screen_present, "no framebuffer");
    int sw = logical_w(), sh = logical_h();
    struct proc *srv = start_server();
    struct proc *cl = proc_create_user("/bin/x12settings", (char *const[]){ "x12settings", "set", "frame_ms", "33", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start x12settings");
    int status = proc_reap(cl);
    ktest_assert(status == 0, "x12settings status 0x%x", status);
    const char *tools[] = { "evtest", "sysmon", "logview", "hexview" };
    int cx = sw / 2, cy = sh / 2;
    for (int i = 0; i < 4; i++) {
        cl = proc_create_user(i == 3 ? "/usr/bin/hexview" : tools[i][0] == 'e' ? "/bin/evtest" : tools[i][0] == 's' ? "/bin/sysmon" : "/bin/logview",
                              (char *const[]){ (char *)tools[i], NULL }, (char *const[]){ NULL }, &kernel_proc);
        ktest_assert(cl != NULL, "cannot start %s", tools[i]);
        int wx = 40 + i * 30, wy = 60 + i * 30;   /* cascade by creation number */
        uint64_t t0 = timer_ms();
        while (pixel(wx + 2, wy - 10) != 0x00ebebeb && timer_ms() - t0 < 4000)
            sleep_ms(50);
        kprintf("gui_tools: %s mapped after %lu ms\n", tools[i], timer_ms() - t0);
        sleep_ms(300);
        ktest_assert(pixel(wx + 2, wy - 10) == 0x00ebebeb, "%s window has an active title bar: %08x", tools[i], pixel(wx + 2, wy - 10));
        if (i == 0) {
            mouse_move_to(&cx, &cy, 40 + 100, 60 + 40, 0);
            mouse_click(1);
            sleep_ms(300);
        }
        alt_key(0x3e);
        status = proc_reap(cl);
        ktest_assert(status == 0, "%s status 0x%x", tools[i], status);
    }
    cl = proc_create_user("/bin/x12settings", (char *const[]){ "x12settings", NULL }, (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start x12settings");
    ktest_wait_idle(1500);
    alt_key(0x3e);
    status = proc_reap(cl);
    ktest_assert(status == 0, "x12settings window status 0x%x", status);
    stop_server(srv);
    kprintf("gui_tools: debugging tools ok\n");
}
KTEST_DEFINE("gui_tools", test_gui_tools);

/* Screenshots, the image viewer and paint. The screenshot program and
 * the Print Screen key write PNG files of the screen; the viewer shows
 * one fitted into its window; paint draws a stroke, undoes and redoes
 * it, and saves; the viewer shows the saved drawing. The post script
 * decodes /shot.png and /drawing.png on the host. Windows cascade by
 * creation number: the first viewer at (40,60), paint at (70,90), the
 * second viewer at (100,120). */
static int count_pixels(int x0, int y0, int w, int h, uint32_t value, uint32_t mask)
{
    int n = 0;
    for (int y = y0; y < y0 + h; y++)
        for (int x = x0; x < x0 + w; x++)
            if ((pixel(x, y) & mask) == (value & mask))
                n++;
    return n;
}

static void wait_active(int wx, int wy, const char *what)
{
    uint64_t t0 = timer_ms();
    while (pixel(wx + 2, wy - 10) != 0x00ebebeb && timer_ms() - t0 < 5000)
        sleep_ms(50);
    ktest_assert(pixel(wx + 2, wy - 10) == 0x00ebebeb, "%s window has an active title bar: %08x", what,
                 pixel(wx + 2, wy - 10));
    ktest_wait_idle(500);
}

static void read_head(const char *path, uint8_t *buf, size_t n)
{
    struct file *f;
    ktest_assert(vfs_open(path, O_RDONLY, 0, &f) == 0, "open %s", path);
    long got = file_read(f, (char *)buf, n);
    file_put(f);
    ktest_assert(got == (long)n, "%s has %ld bytes", path, got);
}

static void test_gui_images(void)
{
    install_app("view");
    install_app("paint");
    ktest_assert(fb_screen_present, "no framebuffer");
    int sw = logical_w(), sh = logical_h();
    ktest_assert(sw >= 800 && sh >= 640, "screen %dx%d too small", sw, sh);
    kprintf("gui_images: screen %lux%lu\n", (unsigned long)fb_screen.width, (unsigned long)fb_screen.height);
    struct proc *srv = start_server();
    int cx = sw / 2, cy = sh / 2;
    mouse_move_to(&cx, &cy, sw - 5, sh / 2, 0);

    /* The screenshot program writes the screen at device resolution. */
    vfs_unlink("/shot.png");
    struct proc *cl = proc_create_user("/bin/screenshot", (char *const[]){ "screenshot", "/shot.png", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start screenshot");
    int status = proc_reap(cl);
    ktest_assert(status == 0, "screenshot status 0x%x", status);
    uint8_t head[26];
    read_head("/shot.png", head, sizeof head);
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    ktest_assert(memcmp(head, sig, 8) == 0 && memcmp(head + 12, "IHDR", 4) == 0, "/shot.png is not a PNG file");
    uint32_t w = (uint32_t)head[16] << 24 | (uint32_t)head[17] << 16 | (uint32_t)head[18] << 8 | head[19];
    uint32_t h = (uint32_t)head[20] << 24 | (uint32_t)head[21] << 16 | (uint32_t)head[22] << 8 | head[23];
    ktest_assert(w == fb_screen.width && h == fb_screen.height, "screenshot %ux%u", (unsigned)w, (unsigned)h);
    ktest_assert(head[24] == 8 && head[25] == 2, "screenshot depth %u colour type %u", head[24], head[25]);

    /* Print Screen opens the capture interface over the frozen screen:
     * the desktop at half brightness outside the selection in the middle,
     * full brightness inside it. Enter saves the selection under a name
     * the program chooses, and its thumbnail appears in the bottom right
     * corner of the desktop area, 16 pixels from the edges, with a light
     * grey frame. */
    ps2kbd_feed_scancode(0xe0);
    ps2kbd_feed_scancode(0x37);
    ps2kbd_feed_scancode(0xe0);
    ps2kbd_feed_scancode(0xb7);
    uint64_t t0 = timer_ms();
    while (pixel(5, sh / 2) != 0x00183040 && timer_ms() - t0 < 5000)
        sleep_ms(50);
    ktest_assert(pixel(5, sh / 2) == 0x00183040, "dimmed outside the selection: %08x", pixel(5, sh / 2));
    ktest_assert(pixel(sw / 2, sh / 2) == 0x00306080, "bright inside the selection: %08x", pixel(sw / 2, sh / 2));
    kprintf("gui_images: capture interface shown\n");
    char pause_arg[8];
    if (cmdline_lookup("pause", pause_arg, sizeof pause_arg) && pause_arg[0] == '1')
        ktest_wait_idle(8000);                 /* screenshots of the interface */
    press_key(0x1c);
    int thumb_y = sh - 28 - 16 - 71;
    t0 = timer_ms();
    while (pixel(sw - 17, thumb_y) != 0x00c0c0c0 && timer_ms() - t0 < 5000)
        sleep_ms(50);
    ktest_assert(pixel(sw - 17, thumb_y) == 0x00c0c0c0, "thumbnail frame: %08x", pixel(sw - 17, thumb_y));
    ktest_assert(pixel(5, sh / 2) == 0x00306080, "interface closed: %08x", pixel(5, sh / 2));
    kprintf("gui_images: thumbnail shown\n");

    /* Super+Shift+4 selects an area: the drag from (100,100) to (300,250)
     * saves it when the button goes up. */
    ps2kbd_feed_scancode(0xe0);
    ps2kbd_feed_scancode(0x5b);
    ps2kbd_feed_scancode(0x2a);
    press_key(0x05);
    ps2kbd_feed_scancode(0xaa);
    ps2kbd_feed_scancode(0xe0);
    ps2kbd_feed_scancode(0xdb);
    t0 = timer_ms();
    while (pixel(5, sh / 2) != 0x00183040 && timer_ms() - t0 < 5000)
        sleep_ms(50);
    ktest_assert(pixel(5, sh / 2) == 0x00183040, "area selection dims the screen: %08x", pixel(5, sh / 2));
    mouse_move_to(&cx, &cy, 100, 100, 0);
    feed_packet(1, 0, 0);
    ktest_wait_idle(100);
    mouse_move_to(&cx, &cy, 200, 180, 1);
    mouse_move_to(&cx, &cy, 300, 250, 1);
    ktest_wait_idle(200);
    ktest_assert(pixel(200, 180) == 0x00306080, "the dragged area is bright: %08x", pixel(200, 180));
    feed_packet(0, 0, 0);
    ktest_wait_idle(2500);

    /* The viewer fits the screenshot into its window: the desktop colour
     * of the image and the dark bars beside it. */
    cl = proc_create_user("/usr/bin/view", (char *const[]){ "view", "/shot.png", NULL },
                          (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start view");
    wait_active(40, 60, "view");
    int desk = count_pixels(40, 60, 640, 480, 0x00306080, 0x00ffffff);
    int bars = count_pixels(40, 60, 640, 480, 0x00303030, 0x00ffffff);
    kprintf("gui_images: viewer shows %d desktop and %d background pixels\n", desk, bars);
    ktest_assert(desk > 20000 && bars > 1000, "fitted screenshot: %d desktop, %d background pixels", desk, bars);
    /* Actual size, zoom in and out, back to the fit, and the next image. */
    press_key(0x02);
    ktest_wait_idle(200);
    press_key(0x0d);
    ktest_wait_idle(200);
    press_key(0x0c);
    ktest_wait_idle(200);
    press_key(0x0b);
    ktest_wait_idle(200);
    ps2kbd_feed_scancode(0xe0);
    ps2kbd_feed_scancode(0x4d);
    ps2kbd_feed_scancode(0xe0);
    ps2kbd_feed_scancode(0xcd);
    ktest_wait_idle(400);
    ktest_assert(count_pixels(40, 60, 640, 480, 0x00306080, 0x00ffffff) > 20000, "viewer after the zoom keys");
    /* Window mode of the capture interface: W retains the active window
     * bright and dims the rest, Enter saves it. */
    uint32_t inside = pixel(340, 260);
    ps2kbd_feed_scancode(0xe0);
    ps2kbd_feed_scancode(0x37);
    ps2kbd_feed_scancode(0xe0);
    ps2kbd_feed_scancode(0xb7);
    t0 = timer_ms();
    while (pixel(5, sh / 2) != 0x00183040 && timer_ms() - t0 < 5000)
        sleep_ms(50);
    press_key(0x11);
    sleep_ms(500);
    ktest_assert(pixel(5, sh / 2) == 0x00183040, "dimmed beside the window: %08x", pixel(5, sh / 2));
    ktest_assert(pixel(340, 260) == inside, "window bright in window mode: %08x, not %08x", pixel(340, 260), inside);
    kprintf("gui_images: window mode shown\n");
    if (cmdline_lookup("pause", pause_arg, sizeof pause_arg) && pause_arg[0] == '1')
        ktest_wait_idle(8000);
    press_key(0x1c);
    ktest_wait_idle(2500);

    /* The active window alone, with its shadow on a transparent
     * background. */
    vfs_unlink("/win.png");
    struct proc *shot = proc_create_user("/bin/screenshot", (char *const[]){ "screenshot", "-w", "/win.png", NULL },
                                         (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(shot != NULL, "cannot start screenshot -w");
    status = proc_reap(shot);
    ktest_assert(status == 0, "screenshot -w status 0x%x", status);
    read_head("/win.png", head, sizeof head);
    w = (uint32_t)head[16] << 24 | (uint32_t)head[17] << 16 | (uint32_t)head[18] << 8 | head[19];
    h = (uint32_t)head[20] << 24 | (uint32_t)head[21] << 16 | (uint32_t)head[22] << 8 | head[23];
    uint32_t sc = fb_screen_scale ? fb_screen_scale : 1;
    kprintf("gui_images: window %ux%u\n", (unsigned)w, (unsigned)h);
    ktest_assert(w >= 640 * sc && h >= 480 * sc && w < 760 * sc && h < 600 * sc, "window image %ux%u", (unsigned)w,
                 (unsigned)h);
    ktest_assert(head[25] == 6, "window image colour type %u", head[25]);
    alt_key(0x3e);
    status = proc_reap(cl);
    ktest_assert(status == 0, "view status 0x%x", status);

    /* Paint: find the white drawing inside the window. */
    vfs_unlink("/drawing.png");
    cl = proc_create_user("/usr/bin/paint", (char *const[]){ "paint", "/drawing.png", NULL },
                          (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start paint");
    int wx = 70, wy = 90;
    wait_active(wx, wy, "paint");
    int top = -1, left = -1;
    for (int y = wy; y < wy + 200 && top < 0; y++)
        if (pixel(wx + 300, y) == 0x00ffffff && pixel(wx + 300, y + 1) == 0x00ffffff && pixel(wx + 300, y + 2) == 0x00ffffff)
            top = y;
    ktest_assert(top >= 0, "no drawing in the paint window");
    for (int x = wx; x < wx + 100 && left < 0; x++)
        if (pixel(x, top + 20) == 0x00ffffff && pixel(x + 1, top + 20) == 0x00ffffff)
            left = x;
    ktest_assert(left >= 0, "no left edge of the drawing");
    kprintf("gui_images: drawing at %d,%d\n", left - wx, top - wy);
    mouse_move_to(&cx, &cy, left + 50, top + 50, 0);
    feed_packet(1, 0, 0);
    ktest_wait_idle(100);
    mouse_move_to(&cx, &cy, left + 250, top + 50, 1);
    ktest_wait_idle(100);
    feed_packet(0, 0, 0);
    ktest_wait_idle(400);
    ktest_assert(pixel(left + 150, top + 50) == 0, "stroke drawn: %08x", pixel(left + 150, top + 50));
    ctrl_key(0x2c);
    ktest_wait_idle(300);
    ktest_assert(pixel(left + 150, top + 50) == 0x00ffffff, "stroke undone: %08x", pixel(left + 150, top + 50));
    ctrl_key(0x15);
    ktest_wait_idle(300);
    ktest_assert(pixel(left + 150, top + 50) == 0, "stroke redone: %08x", pixel(left + 150, top + 50));
    ctrl_key(0x1f);
    ktest_wait_idle(1000);
    alt_key(0x3e);
    status = proc_reap(cl);
    ktest_assert(status == 0, "paint status 0x%x", status);
    read_head("/drawing.png", head, sizeof head);
    ktest_assert(memcmp(head, sig, 8) == 0, "/drawing.png is not a PNG file");

    /* The codecs command converts the drawing to BMP through the codec
     * modules, and the viewer shows the BMP file: the stroke shows as
     * black pixels in the canvas, below the tool bar and above the status
     * bar. */
    vfs_unlink("/drawing.bmp");
    cl = proc_create_user("/bin/codecs", (char *const[]){ "codecs", "convert", "/drawing.png", "/drawing.bmp", NULL },
                          (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start codecs");
    status = proc_reap(cl);
    ktest_assert(status == 0, "codecs convert status 0x%x", status);
    read_head("/drawing.bmp", head, 2);
    ktest_assert(head[0] == 'B' && head[1] == 'M', "/drawing.bmp is not a BMP file");
    cl = proc_create_user("/usr/bin/view", (char *const[]){ "view", "/drawing.bmp", NULL },
                          (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start the second view");
    wait_active(100, 120, "second view");
    int dark = count_pixels(110, 120 + 80, 620, 300, 0, 0x00f0f0f0);
    kprintf("gui_images: saved stroke shows %d dark pixels\n", dark);
    ktest_assert(dark > 200, "saved stroke in the viewer: %d dark pixels", dark);
    alt_key(0x3e);
    status = proc_reap(cl);
    ktest_assert(status == 0, "second view status 0x%x", status);
    stop_server(srv);
    vfs_sync();
    kprintf("gui_images: images ok\n");
}
KTEST_DEFINE("gui_images", test_gui_images);

/* The hex viewer on a file of known bytes: Find text and Find bytes
 * select the matches, F3 finds the second byte sequence, and the program
 * reports every match on standard output. */
static void test_gui_hexview(void)
{
    install_app("hexview");
    ktest_assert(fb_screen_present, "no framebuffer");
    static char data[4096];
    for (int i = 0; i < 4096; i++)
        data[i] = (char)(i * 7);
    memcpy(data + 0x700, "NEEDLE", 6);
    memcpy(data + 0x900, "\xde\xad\xbe\xef", 4);
    memcpy(data + 0xa00, "\xde\xad\xbe\xef", 4);
    struct file *f;
    vfs_unlink("/hex.bin");
    ktest_assert(vfs_open("/hex.bin", O_WRONLY | O_CREAT, 0644, &f) == 0, "create /hex.bin");
    ktest_assert(file_write(f, data, sizeof data) == (long)sizeof data, "write /hex.bin");
    file_put(f);
    struct proc *srv = start_server();
    struct proc *cl = proc_create_user("/usr/bin/hexview", (char *const[]){ "hexview", "/hex.bin", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start hexview");
    wait_active(40, 60, "hexview");
    ctrl_key(0x21);                     /* Ctrl+F */
    ktest_wait_idle(600);
    type_line("NEEDLE\n");
    ktest_wait_idle(600);
    ctrl_key(0x30);                     /* Ctrl+B */
    ktest_wait_idle(600);
    type_line("de ad be ef\n");
    ktest_wait_idle(600);
    press_key(0x3d);                    /* F3 */
    ktest_wait_idle(600);
    alt_key(0x3e);
    int status = proc_reap(cl);
    ktest_assert(status == 0, "hexview status 0x%x", status);
    stop_server(srv);
    vfs_unlink("/hex.bin");
    kprintf("gui_hexview: hex viewer ok\n");
}
KTEST_DEFINE("gui_hexview", test_gui_hexview);

/* The kernel log viewer with a level, a subsystem and a text filter from
 * its options: a warning written before it starts and an error written
 * while it runs are shown, an info line is not, and Save writes the rows
 * shown. */
static bool text_contains(const char *hay, const char *needle)
{
    size_t n = strlen(needle);
    for (; *hay; hay++)
        if (strncmp(hay, needle, n) == 0)
            return true;
    return false;
}

static void test_gui_logview(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    klog_print(LOG_INFO, "ktest", "logview info marker");
    klog_print(LOG_WARN, "ktest", "logview warning marker");
    vfs_unlink("/root/klog.txt");
    struct proc *srv = start_server();
    struct proc *cl = proc_create_user("/bin/logview",
                                       (char *const[]){ "logview", "-l", "warning", "-s", "ktest", "-f", "MARKER", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start logview");
    wait_active(40, 60, "logview");
    klog_print(LOG_ERROR, "ktest", "logview late error marker");
    ktest_wait_idle(500);
    ctrl_key(0x1f);                     /* Ctrl+S */
    ktest_wait_idle(600);
    type_line("\n");
    ktest_wait_idle(600);
    struct file *f;
    ktest_assert(vfs_open("/root/klog.txt", O_RDONLY, 0, &f) == 0, "open /root/klog.txt");
    static char buf[2048];
    long n = file_read(f, buf, sizeof buf - 1);
    file_put(f);
    buf[n > 0 ? n : 0] = '\0';
    kprintf("gui_logview: saved file:\n%s", buf);
    ktest_assert(text_contains(buf, "] [W ktest] logview warning marker"), "warning row missing");
    ktest_assert(text_contains(buf, "] [E ktest] logview late error marker"), "late error row missing");
    ktest_assert(!text_contains(buf, "info marker"), "info row shown");
    int lines = 0;
    for (char *p = buf; *p; p++)
        lines += *p == '\n';
    ktest_assert(lines == 2, "saved %d lines", lines);
    alt_key(0x3e);
    int status = proc_reap(cl);
    ktest_assert(status == 0, "logview status 0x%x", status);
    stop_server(srv);
    vfs_unlink("/root/klog.txt");
    kprintf("gui_logview: log viewer ok\n");
}
KTEST_DEFINE("gui_logview", test_gui_logview);

/* The system monitor: /dev/cpustat counts the ticks of every CPU, the
 * process filter and Ctrl+K kill a sleeping process, and the Resources tab
 * draws its graphs. */
static void cpustat_sum(uint64_t *busy, uint64_t *idle, unsigned *cpus)
{
    static char text[1024];
    proc_format_cpustat(text, sizeof text);
    ktest_assert(text_contains(text, "CPU USER SYSTEM IDLE\n"), "cpustat header: %s", text);
    *busy = *idle = 0;
    *cpus = 0;
    for (char *p = strchr(text, '\n') + 1; *p; (*cpus)++) {
        uint64_t v[4];
        for (int k = 0; k < 4; k++) {
            v[k] = strtoull(p, &p, 10);
            if (*p == ' ')
                p++;
        }
        ktest_assert(*p == '\n', "cpustat line format: %s", text);
        p++;
        *busy += v[1] + v[2];
        *idle += v[3];
    }
}

static uint64_t cpu_ticks_sum(void)
{
    uint64_t sum = 0;
    for (unsigned id = 0; id < smp_cpu_count(); id++)
        sum += __atomic_load_n(&cpu_by_id(id)->ticks, __ATOMIC_RELAXED);
    return sum;
}

static void test_gui_sysmon(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    uint64_t busy0, idle0, busy1, idle1;
    unsigned cpus;
    uint64_t ticks0 = cpu_ticks_sum();
    cpustat_sum(&busy0, &idle0, &cpus);
    ktest_assert(cpus == smp_cpu_count(), "cpustat lists %u of %u cpus", cpus, smp_cpu_count());
    sleep_ms(500);
    cpustat_sum(&busy1, &idle1, &cpus);
    uint64_t ticks = cpu_ticks_sum() - ticks0;
    uint64_t counted = busy1 - busy0 + idle1 - idle0;
    kprintf("gui_sysmon: %u cpus, %lu of %lu ticks counted, %lu idle\n", cpus, counted, ticks, idle1 - idle0);
    /* Every timer interrupt after the scheduler start increments exactly
     * one of the three counters of its CPU.  The two samples are not
     * atomic, and a few ticks may differ between them. */
    ktest_assert(counted + 4 * cpus >= ticks && counted <= ticks + 4 * cpus, "cpustat counted %lu of %lu ticks",
                 counted, ticks);
    ktest_assert(idle1 > idle0, "no idle ticks");

    struct proc *srv = start_server();
    struct proc *victim = proc_create_user("/bin/sleep", (char *const[]){ "sleep", "100", NULL },
                                           (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(victim != NULL, "cannot start sleep");
    struct proc *cl = proc_create_user("/bin/sysmon", (char *const[]){ "sysmon", NULL }, (char *const[]){ NULL },
                                       &kernel_proc);
    ktest_assert(cl != NULL, "cannot start sysmon");
    wait_active(40, 60, "sysmon");
    ktest_wait_idle(500);
    /* Shift+Tab moves the focus from the process table to the filter. */
    ps2kbd_feed_scancode(0x2a);
    press_key(0x0f);
    ps2kbd_feed_scancode(0xaa);
    ktest_wait_idle(200);
    type_line("sleep");
    ktest_wait_idle(300);
    press_key(0x0f);                    /* Tab returns to the table. */
    ktest_wait_idle(200);
    ps2kbd_feed_scancode(0xe0);         /* Home selects the only row. */
    ps2kbd_feed_scancode(0x47);
    ps2kbd_feed_scancode(0xe0);
    ps2kbd_feed_scancode(0xc7);
    ktest_wait_idle(300);
    ctrl_key(0x25);                     /* Ctrl+K */
    int status = proc_reap(victim);
    kprintf("gui_sysmon: sleep status 0x%x\n", status);
    ktest_assert(status != 0, "sleep exited normally");

    ctrl_key(0x03);                     /* Ctrl+2 shows the Resources tab. */
    ktest_wait_idle(2500);
    int accent = 0;
    for (int y = 60; y < 60 + 540; y++)
        for (int x = 40; x < 40 + 760; x++)
            accent += pixel(x, y) == 0x003c78c8;
    kprintf("gui_sysmon: %d accent pixels in the graphs\n", accent);
    ktest_assert(accent > 300, "graphs drawn: %d accent pixels", accent);
    alt_key(0x3e);
    status = proc_reap(cl);
    ktest_assert(status == 0, "sysmon status 0x%x", status);
    stop_server(srv);
    kprintf("gui_sysmon: system monitor ok\n");
}
KTEST_DEFINE("gui_sysmon", test_gui_sysmon);

/* sysinfo (docs/design/sysinfo.md): the window opens with the system node
 * selected, the Down key selects the firmware node, F5 reads /dev/devices
 * again and Alt+F4 closes the window. The program reports each step on
 * standard output, which the case checks. */
static void test_gui_sysinfo(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    struct proc *srv = start_server();
    struct proc *cl = proc_create_user("/bin/sysinfo", (char *const[]){ "sysinfo", NULL }, (char *const[]){ NULL },
                                       &kernel_proc);
    ktest_assert(cl != NULL, "cannot start sysinfo");
    wait_active(40, 60, "sysinfo");
    ktest_wait_idle(500);
    ps2kbd_feed_scancode(0xe0);         /* Down */
    ps2kbd_feed_scancode(0x50);
    ps2kbd_feed_scancode(0xe0);
    ps2kbd_feed_scancode(0xd0);
    ktest_wait_idle(300);
    press_key(0x3f);                    /* F5 */
    ktest_wait_idle(500);
    alt_key(0x3e);                      /* Alt+F4 */
    int status = proc_reap(cl);
    ktest_assert(status == 0, "sysinfo status 0x%x", status);
    stop_server(srv);
    kprintf("gui_sysinfo: system information ok\n");
}
KTEST_DEFINE("gui_sysinfo", test_gui_sysinfo);

/* L4: translated programs.  The panel runs with LANG=ja_JP.UTF-8, and
 * sysmon runs once with LANG=fr_FR.UTF-8 and once with LANG=ja_JP.UTF-8.
 * The compositor log names the translated window titles, and the launcher
 * menu draws its Japanese titles with the CJK font. */
static void test_gui_locale(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    char *const fr[] = { "LANG=fr_FR.UTF-8", NULL }, *const ja[] = { "LANG=ja_JP.UTF-8", NULL };
    struct proc *srv = start_x12((char *const[]){ "x12", "-s", NULL });
    struct proc *panel = proc_create_user("/bin/panel", (char *const[]){ "panel", NULL }, ja, &kernel_proc);
    ktest_assert(panel != NULL, "cannot start the panel");
    ktest_wait_idle(800);
    char *const *envs[] = { fr, ja };
    for (int i = 0; i < 2; i++) {
        struct proc *cl = proc_create_user("/bin/sysmon", (char *const[]){ "sysmon", NULL }, envs[i], &kernel_proc);
        ktest_assert(cl != NULL, "cannot start sysmon");
        sleep_ms(1500);
        alt_key(0x3e);
        int status = proc_reap(cl);
        ktest_assert(status == 0, "sysmon status 0x%x", status);
    }
    int sh = logical_h(), cx = logical_w() / 2, cy = sh / 2;
    mouse_move_to(&cx, &cy, 30, sh - 14, 0);
    mouse_click(1);
    ktest_wait_idle(1500);
    kprintf("gui_locale: launcher menu open\n");
    press_key(0x01);
    ktest_wait_idle(300);
    signal_send(panel, SIGTERM);
    proc_reap(panel);
    signal_send(srv, SIGTERM);
    proc_reap(srv);
    kprintf("gui_locale: locale ok\n");
}
KTEST_DEFINE("gui_locale", test_gui_locale);

/* The protocol viewer: the text mode prints the requests and events of
 * the clock while it connects, then the window records a second clock
 * and reports what it received when it is closed. Windows cascade by
 * creation number, so the first clock is at (40,60), the viewer at
 * (70,90) and the second clock at (100,120). */
static void test_gui_wireview(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    int sw = logical_w(), sh = logical_h();
    struct proc *srv = start_server();
    struct proc *trace = proc_create_user("/bin/wireview", (char *const[]){ "wireview", "-t", "-n", "40", NULL },
                                          (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(trace != NULL, "cannot start wireview -t");
    sleep_ms(500);
    struct proc *clock = proc_create_user("/bin/clock", (char *const[]){ "clock", NULL }, (char *const[]){ NULL },
                                          &kernel_proc);
    ktest_assert(clock != NULL, "cannot start clock");
    int status = proc_reap(trace);
    ktest_assert(status == 0, "wireview -t status 0x%x", status);
    sleep_ms(500);
    alt_key(0x3e);
    status = proc_reap(clock);
    ktest_assert(status == 0, "clock status 0x%x", status);

    struct proc *view = proc_create_user("/bin/wireview", (char *const[]){ "wireview", NULL }, (char *const[]){ NULL },
                                         &kernel_proc);
    ktest_assert(view != NULL, "cannot start wireview");
    int wx = 70, wy = 90;
    uint64_t t0 = timer_ms();
    while (pixel(wx + 2, wy - 10) != 0x00ebebeb && timer_ms() - t0 < 4000)
        sleep_ms(50);
    ktest_assert(pixel(wx + 2, wy - 10) == 0x00ebebeb, "wireview window has an active title bar: %08x",
                 pixel(wx + 2, wy - 10));
    clock = proc_create_user("/bin/clock", (char *const[]){ "clock", NULL }, (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(clock != NULL, "cannot start the second clock");
    sleep_ms(1500);
    kprintf("gui_wireview: viewer recording\n");
    sleep_ms(1500);
    alt_key(0x3e);
    status = proc_reap(clock);
    ktest_assert(status == 0, "second clock status 0x%x", status);
    /* Focus the viewer by its title bar, then close it. */
    int cx = sw / 2, cy = sh / 2;
    mouse_move_to(&cx, &cy, wx + 200, wy - 10, 0);
    mouse_click(1);
    sleep_ms(300);
    alt_key(0x3e);
    status = proc_reap(view);
    ktest_assert(status == 0, "wireview status 0x%x", status);
    stop_server(srv);
    kprintf("gui_wireview: protocol viewer ok\n");
}
KTEST_DEFINE("gui_wireview", test_gui_wireview);

/* The Unicode viewer walks the whole code space once at startup to record
 * which code points its font covers, so this test also measures that the
 * window still appears promptly. */
static void test_gui_unicode(void)
{
    install_app("unicode");
    ktest_assert(fb_screen_present, "no framebuffer");
    int sw = logical_w(), sh = logical_h();
    struct proc *srv = start_server();
    struct proc *cl = proc_create_user("/usr/bin/unicode", (char *const[]){ "unicode", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start unicode");
    int active = 0;
    for (int i = 0; i < 120 && !active; i++) {
        sleep_ms(100);
        active = pixel(42, 50) == 0x00ebebeb;
    }
    ktest_assert(active, "unicode window has an active title bar: %08x", pixel(42, 50));
    /* Click a cell in the grid, which starts below the toolbar and the
     * preview panel, and let the selection repaint. */
    int cx = sw / 2, cy = sh / 2;
    mouse_move_to(&cx, &cy, 40 + 100, 60 + 230, 0);
    mouse_click(1);
    ktest_wait_idle(400);
    ktest_assert(pixel(42, 50) == 0x00ebebeb, "unicode window survives a grid click: %08x", pixel(42, 50));
    alt_key(0x3e);
    int status = proc_reap(cl);
    ktest_assert(status == 0, "unicode status 0x%x", status);
    stop_server(srv);
    kprintf("gui_unicode: viewer ok\n");
}
KTEST_DEFINE("gui_unicode", test_gui_unicode);

/* Desktop layer (wallpaper, icons, context menus) and the settings
 * application: the desktop starts as a solid colour, a wallpaper set with
 * "settings set" replaces it, a double
 * click on the Clock icon starts the clock, right clicks open the two
 * context menus, "settings set" changes the configuration file which the
 * desktop applies, and the settings window opens and closes. */
/* Parse "PID PPID PGID STATE ..." of a /dev/proc row. */
static bool ksscanf_row(const char *line, int *pid, int *ppid, int *pgid, char *state, size_t size)
{
    const char *p = line;
    int *fields[3] = { pid, ppid, pgid };
    for (int i = 0; i < 3; i++) {
        while (*p == ' ')
            p++;
        if (*p < '0' || *p > '9')
            return false;
        char *end;
        *fields[i] = (int)strtoull(p, &end, 10);
        p = end;
    }
    while (*p == ' ')
        p++;
    size_t k = 0;
    while (*p && *p != ' ' && k + 1 < size)
        state[k++] = *p++;
    state[k] = '\0';
    return true;
}

/* Zombie children of ppid in the process table; ppid 0 is the kernel
 * process, which adopts orphans when no init runs. */
static int zombies_of(int ppid)
{
    static char text[8192];
    proc_format_table(text, sizeof text);
    int n = 0;
    for (char *line = text; line && *line; ) {
        char *nl = strchr(line, '\n');
        if (nl)
            *nl = '\0';
        int pid, parent, pgid;
        char state[16];
        if (ksscanf_row(line, &pid, &parent, &pgid, state, sizeof state) && parent == ppid && strcmp(state, "zombie") == 0)
            n++;
        line = nl ? nl + 1 : NULL;
    }
    return n;
}

static void test_gui_desktop(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    int sw = logical_w(), sh = logical_h();
    struct proc *srv = start_compositor();
    struct proc *panel = proc_create_user("/bin/panel", (char *const[]){ "panel", NULL }, (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(panel != NULL, "cannot start the panel");
    ktest_wait_idle(500);
    struct proc *desktop = proc_create_user("/bin/desktop", (char *const[]){ "desktop", NULL }, (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(desktop != NULL, "cannot start the desktop");
    ktest_wait_idle(2000);
    /* Sample away from the cursor, which sits at the centre. */
    uint32_t mid = pixel(sw / 2 + 100, sh / 2 + 50);
    ktest_assert(mid == 0x00306080, "solid desktop colour by default: %08x", mid);
    ktest_assert(pixel(sw / 2, sh - 14) == 0x0023272c, "panel above the desktop: %08x", pixel(sw / 2, sh - 14));
    /* A wallpaper set from the command line is applied within a second. */
    struct proc *cl = proc_create_user("/bin/settings", (char *const[]){ "settings", "set", "wallpaper", "/usr/share/wallpapers/default.png", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start settings");
    int status = proc_reap(cl);
    ktest_assert(status == 0, "settings status 0x%x", status);
    /* The desktop decodes and scales the image, which takes several
     * seconds under TCG. */
    wait_pixel(sw / 2 + 100, sh / 2 + 50, 0x00306080, 0, 10000);
    mid = pixel(sw / 2 + 100, sh / 2 + 50);
    ktest_assert(mid != 0x00306080, "wallpaper drawn in the middle of the screen: %08x", mid);
    int cx = sw / 2, cy = sh / 2;
    /* Double click on the first icon (Clock.app). */
    mouse_move_to(&cx, &cy, 55, 50, 0);
    mouse_click(1);
    mouse_click(1);
    ktest_wait_idle(1500);
    ktest_assert(pixel(40 + 2, 60 - 10) == 0x00ebebeb, "clock window opened from the desktop: %08x", pixel(42, 50));
    alt_key(0x3e);
    ktest_wait_idle(600);
    /* The desktop starts programs as children of init, so the clock is
     * not its zombie; here the kernel process adopted and retains it. */
    ktest_assert(zombies_of(desktop->pid) == 0, "the desktop left a zombie child");
    ktest_assert(zombies_of(0) == 1, "the clock became the kernel's orphan: %d", zombies_of(0));
    /* Context menus: on the desktop, then on the fourth icon (readme.txt). */
    mouse_move_to(&cx, &cy, 600, 300, 0);
    mouse_click(2);
    ktest_wait_idle(300);
    mouse_move_to(&cx, &cy, 800, 500, 0);
    mouse_click(1);
    ktest_wait_idle(300);
    mouse_move_to(&cx, &cy, 55, 300, 0);
    mouse_click(2);
    ktest_wait_idle(300);
    mouse_move_to(&cx, &cy, 800, 500, 0);
    mouse_click(1);
    ktest_wait_idle(300);
    /* Another setting from the command line. */
    cl = proc_create_user("/bin/settings", (char *const[]){ "settings", "set", "wallpaper_mode", "tile", NULL },
                          (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start settings");
    status = proc_reap(cl);
    ktest_assert(status == 0, "settings status 0x%x", status);
    ktest_wait_idle(1800);
    /* The settings window (toplevel 2 cascades to 70,90). */
    cl = proc_create_user("/bin/settings", (char *const[]){ "settings", NULL }, (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start settings");
    ktest_wait_idle(1800);
    ktest_assert(pixel(70 + 2, 90 - 10) == 0x00ebebeb, "settings window has an active title bar: %08x", pixel(72, 80));
    alt_key(0x3e);
    status = proc_reap(cl);
    ktest_assert(status == 0, "settings window status 0x%x", status);
    signal_send(desktop, SIGTERM);
    proc_reap(desktop);
    signal_send(panel, SIGTERM);
    proc_reap(panel);
    signal_send(srv, SIGTERM);
    proc_reap(srv);
    kprintf("gui_desktop: desktop ok\n");
}
KTEST_DEFINE("gui_desktop", test_gui_desktop);
