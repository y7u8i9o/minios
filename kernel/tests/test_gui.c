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
#include <arch/boot.h>
#include <fs/vfs.h>
#include <lib/string.h>
#include <lib/cmdline.h>
#include <lib/crc32.h>
#include <console.h>
#include <errno.h>
#include <drivers/timer.h>
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
    /* Wait until the program has drawn and is holding the display. */
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
    sleep_ms(200);
    ktest_assert(pixel(0, 0) == 0x00306080, "desktop pixel %08x", pixel(0, 0));

    struct proc *cl = proc_create_user("/bin/guitest", (char *const[]){ "guitest", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start guitest");
    sleep_ms(800);
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
    sleep_ms(100);
    /* Click alpha's contents left of beta (whose frame and its resize
     * border start at x 60): it comes to the front and gets focus. */
    mouse_move_to(&cx, &cy, 50, 200, 0);
    mouse_click(1);
    sleep_ms(100);
    ktest_assert(pixel(40 + 2, 60 - 10) == 0x00ebebeb, "alpha title bar active after click %08x", pixel(42, 50));
    ktest_assert(pixel(70 + 50, 90 + 40) == 0x00dcdcdc, "alpha now covers beta: %08x", pixel(120, 130));
    /* Drag alpha by its title bar 100 pixels to the right. */
    mouse_move_to(&cx, &cy, 150, 50, 0);
    feed_packet(1, 0, 0);
    sleep_ms(50);
    mouse_move_to(&cx, &cy, 250, 50, 1);
    feed_packet(0, 0, 0);
    sleep_ms(200);
    ktest_assert(pixel(140 + 5, 60 + 100) == 0x00dcdcdc, "alpha moved: %08x", pixel(145, 160));
    ktest_assert(pixel(40 + 5, 60 + 100) == 0x00306080 || pixel(40 + 5, 60 + 100) == 0x00c8f0c8,
                 "old alpha area repainted: %08x", pixel(45, 160));
    /* Close alpha (the close button at the right of its header bar,
     * 19 px from the edge, 16 px above the contents), then beta. */
    mouse_move_to(&cx, &cy, 140 + 300 - 19, 60 - 16, 0);
    mouse_click(1);
    sleep_ms(200);
    mouse_move_to(&cx, &cy, 70 + 240 - 19, 90 - 16, 0);
    mouse_click(1);
    int status = proc_reap(cl);
    ktest_assert(status == 0, "guitest status 0x%x", status);
    sleep_ms(200);
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
     * hold 50 columns, 335 - 6 = 329 hold 19 rows); the shell sees the
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

/* Geometry of the launcher popup, which the panel builds from
 * user/etc/launcher: one 24 px row per entry inside 6 px of padding,
 * opened above the 28 px panel.  Keep these in step with that file. */
#define LAUNCHER_ENTRIES 10
#define LAUNCHER_CLOCK   2     /* index of Clock=/bin/clock */
#define LAUNCHER_TOP(sh) ((sh) - 28 + 4 - (LAUNCHER_ENTRIES * 24 + 12))
#define LAUNCHER_ROW(sh, i) (LAUNCHER_TOP(sh) + 6 + (i) * 24 + 12)





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
    sleep_ms(800);
    /* Window 1 "alpha" 300x200 at (40,60). Drag its bottom right
     * resize border (in the shadow outside the frame) by (100,50). */
    int cx = sw / 2, cy = sh / 2;
    mouse_move_to(&cx, &cy, 40 + 300 + 3, 60 + 200 + 3, 0);
    feed_packet(1, 0, 0);
    sleep_ms(50);
    mouse_move_to(&cx, &cy, 40 + 400 + 3, 60 + 250 + 3, 1);
    feed_packet(0, 0, 0);
    sleep_ms(500);
    ktest_assert(pixel(40 + 350, 60 + 220) == 0x0040c040, "resized contents %08x", pixel(390, 280));
    ktest_assert(pixel(40 + 2, 60 - 10) == 0x00ebebeb, "title bar after resize %08x", pixel(42, 50));
    /* Maximize: second button from the right of the header bar (22 px
     * buttons 6 px apart, the close button 8 px from the right edge;
     * the bar is 30 px tall, its buttons centred 16 px above the
     * contents). */
    mouse_move_to(&cx, &cy, 40 + 400 - 47, 60 - 16, 0);
    mouse_click(1);
    sleep_ms(500);
    ktest_assert(pixel(sw - 10, dh - 10) == 0x0040c040, "maximized contents %08x", pixel(sw - 10, dh - 10));
    ktest_assert(pixel(sw / 2, sh - 14) == 0x0023272c || pixel(sw / 2, sh - 14) == 0x002e343b,
                 "task bar visible %08x", pixel(sw / 2, sh - 14));
    /* Restore through the same button, now at the top right of the
     * screen (the maximized frame starts at the top left corner). */
    mouse_move_to(&cx, &cy, sw - 47, 14, 0);
    mouse_click(1);
    sleep_ms(500);
    ktest_assert(pixel(sw - 10, dh - 10) == 0x00306080, "desktop after restore %08x", pixel(sw - 10, dh - 10));
    ktest_assert(pixel(40 + 350, 60 + 220) == 0x0040c040, "restored contents %08x", pixel(390, 280));
    /* Close button of the header bar. */
    mouse_move_to(&cx, &cy, 40 + 400 - 19, 60 - 16, 0);
    mouse_click(1);
    int status = proc_reap(cl);
    ktest_assert(status == 0, "guitest status 0x%x", status);
    sleep_ms(200);
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
    sleep_ms(800);
    /* alpha 300x200 at (40,60), beta 240x160 at (70,90) on top and focused. */
    int cx = sw / 2, cy = sh / 2;
    mouse_move_to(&cx, &cy, 100, 120, 0);
    feed_packet_wheel(0, 0, 0, 1);
    sleep_ms(150);
    /* Alt+Tab brings alpha, the lowest window, to the top. */
    alt_key(0x0f);
    ktest_assert(pixel(40 + 2, 60 - 10) == 0x00ebebeb, "alpha active after alt-tab %08x", pixel(42, 50));
    ktest_assert(pixel(70 + 50, 90 + 40) == 0x00dcdcdc, "alpha above beta %08x", pixel(120, 130));
    /* Minimize alpha: the third button from the right of its header
     * bar (22 px buttons 6 px apart, close 8 px from the edge). */
    mouse_move_to(&cx, &cy, 40 + 300 - 75, 60 - 16, 0);
    mouse_click(1);
    sleep_ms(300);
    ktest_assert(pixel(45, 160) == 0x00306080, "alpha hidden %08x", pixel(45, 160));
    ktest_assert(pixel(70 + 50, 90 + 40) == 0x00ff0000, "beta visible again %08x", pixel(120, 130));
    /* Its task bar button (the first one) restores it. */
    mouse_move_to(&cx, &cy, 64 + 12 + 40, sh - 14, 0);
    mouse_click(1);
    sleep_ms(300);
    ktest_assert(pixel(45, 160) == 0x00dcdcdc, "alpha restored %08x", pixel(45, 160));
    /* Bring beta to the top and maximize it: alpha is fully covered. */
    alt_key(0x0f);
    mouse_move_to(&cx, &cy, 70 + 240 - 47, 90 - 16, 0);
    mouse_click(1);
    sleep_ms(500);
    ktest_assert(pixel(45, 160) == 0x0040c040, "beta covers alpha %08x", pixel(45, 160));
    /* The launcher menu starts the clock; Alt+F4 closes it. */
    mouse_move_to(&cx, &cy, 30, sh - 14, 0);
    mouse_click(1);
    sleep_ms(200);
    mouse_move_to(&cx, &cy, 40, LAUNCHER_ROW(sh, LAUNCHER_CLOCK), 0);
    mouse_click(1);
    sleep_ms(1500);
    alt_key(0x3e);
    sleep_ms(300);
    /* Alt+F4 closes beta, then alpha; the client exits. */
    alt_key(0x3e);
    sleep_ms(300);
    alt_key(0x3e);
    int status = proc_reap(cl);
    ktest_assert(status == 0, "guitest status 0x%x", status);
    sleep_ms(200);
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
    sleep_ms(1000);
    int cx = sw / 2, cy = sh / 2;
    mouse_move_to(&cx, &cy, 40 + 100, 60 + 51, 0);
    mouse_click(1);
    press_key(0x1e);
    press_key(0x30);
    sleep_ms(150);
    ctrl_key(0x1e);
    ctrl_key(0x2e);
    mouse_move_to(&cx, &cy, 40 + 60, 60 + 150, 0);
    mouse_click(1);
    ctrl_key(0x2f);
    press_key(0x1c);
    press_key(0x2d);
    sleep_ms(150);
    mouse_move_to(&cx, &cy, 40 + 277, 60 + 200, 0);
    feed_packet_wheel(0, 0, 0, 1);
    sleep_ms(150);
    mouse_move_to(&cx, &cy, 40 + 12, 60 + 83, 0);
    mouse_click(1);
    mouse_move_to(&cx, &cy, 40 + 300, 60 + 150, 0);
    mouse_click(1);
    ps2kbd_feed_scancode(0xe0);
    ps2kbd_feed_scancode(0x4f);
    ps2kbd_feed_scancode(0xe0);
    ps2kbd_feed_scancode(0xcf);
    sleep_ms(150);
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
    sleep_ms(1000);
    int cx = sw / 2, cy = sh / 2;
    mouse_move_to(&cx, &cy, 40 + 100, 60 + 19, 0);
    mouse_click(1);
    sleep_ms(200);
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
    sleep_ms(200);
    mouse_move_to(&cx, &cy, 40 + 100, 60 + 149, 0);
    mouse_click(1);
    int status = proc_reap(cl);
    ktest_assert(status == 0, "widgettest status 0x%x", status);
    stop_server(srv);
    kprintf("gui_controls: controls ok\n");
}
KTEST_DEFINE("gui_controls", test_gui_controls);

/* M22: gedit types C source, highlights the keyword, saves with Ctrl+S. */
static void test_gui_editor(void)
{
    install_app("gedit");
    ktest_assert(fb_screen_present, "no framebuffer");
    struct proc *srv = start_server();
    vfs_unlink("/gedit.c");
    struct proc *cl = proc_create_user("/home/.local/bin/gedit", (char *const[]){ "gedit", "/gedit.c", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start gedit");
    sleep_ms(1500);
    type_line("int x;");
    sleep_ms(400);
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
    sleep_ms(400);
    struct file *f;
    ktest_assert(vfs_open("/gedit.c", O_RDONLY, 0, &f) == 0, "open /gedit.c");
    char buf[32];
    long n = file_read(f, buf, sizeof buf - 1);
    file_put(f);
    buf[n > 0 ? n : 0] = '\0';
    ktest_assert(strcmp(buf, "int x;") == 0, "saved text '%s'", buf);
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
    sleep_ms(1500);
    int cx = sw / 2, cy = sh / 2;
    mouse_move_to(&cx, &cy, 70 + 6 + 194, 90 + 6 + 15, 0);
    mouse_click(1);
    sleep_ms(300);
    ktest_assert(pixel(70 + 6 + 100, 90 + 6 + 15) == 0x00d0d0d0, "hovered button colour %08x", pixel(176, 111));
    press_key(0x0f);                    /* Tab: focus moves to the field */
    sleep_ms(150);
    press_key(0x23);                    /* h */
    press_key(0x17);                    /* i */
    sleep_ms(300);
    /* The list: third entry. */
    mouse_move_to(&cx, &cy, 70 + 100, 90 + 6 + 30 + 6 + 26 + 6 + 1 + 2 * 21 + 10, 0);
    mouse_click(1);
    sleep_ms(300);
    alt_key(0x3e);                      /* close "one" */
    sleep_ms(300);
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
    struct proc *cl = proc_create_user("/home/.local/bin/calc", (char *const[]){ "calc", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start calc");
    sleep_ms(1200);
    ktest_assert(pixel(42, 50) == 0x00ebebeb,
                 "calculator window has an active title bar: %08x", pixel(42, 50));

    press_key(0x04);                    /* 3 ENTER 4 + */
    press_key(0x1c);
    press_key(0x05);
    ps2kbd_feed_scancode(0x2a);
    press_key(0x0d);
    ps2kbd_feed_scancode(0xaa);
    sleep_ms(300);

    int cx = sw / 2, cy = sh / 2;
    mouse_move_to(&cx, &cy, 150, 79, 0);  /* mode combo in the first row */
    feed_packet_wheel(0, 0, 0, 1);        /* RPN -> Algebraic */
    sleep_ms(300);
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
    sleep_ms(300);

    alt_key(0x3e);
    int status = proc_reap(cl);
    ktest_assert(status == 0, "calc status 0x%x", status);
    stop_server(srv);
    kprintf("gui_calc: RPN and algebraic input ok\n");
}
KTEST_DEFINE("gui_calc", test_gui_calc);

/* The progressive Mandelbrot plotter: a 640x480 window at (40,60) whose
 * canvas lies below the tool bar. The centre of the home view (-0.6) is
 * inside the set and painted black; the left edge (-2.2) is outside and
 * coloured. A wheel step zooms in and starts a second render; Escape
 * ends the program. */
static void test_gui_mandel(void)
{
    install_app("mandel");
    ktest_assert(fb_screen_present, "no framebuffer");
    int sw = logical_w(), sh = logical_h();
    struct proc *srv = start_server();
    struct proc *cl = proc_create_user("/home/.local/bin/mandel", (char *const[]){ "mandel", NULL },
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
    sleep_ms(1500);
    press_key(0x01);                        /* escape */
    int status = proc_reap(cl);
    ktest_assert(status == 0, "mandel status 0x%x", status);
    stop_server(srv);
    kprintf("gui_mandel: progressive plotter ok\n");
}
KTEST_DEFINE("gui_mandel", test_gui_mandel);

/* Drag performance: 200 motion packets with the button held on the
 * terminal window's title bar; the elapsed time is logged and the
 * server reports compositions slower than 20 ms. */
static void test_gui_drag(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    int sw = logical_w(), sh = logical_h();
    struct proc *srv = start_server();
    struct proc *cl = proc_create_user("/bin/term", (char *const[]){ "term", NULL }, (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start term");
    sleep_ms(1500);
    int cx = sw / 2, cy = sh / 2;
    mouse_move_to(&cx, &cy, 40 + 300, 60 - 10, 0);
    feed_packet(1, 0, 0);
    sleep_ms(100);
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
    sleep_ms(300);
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
    struct proc *srv = proc_create_user("/bin/x12", (char *const[]){ "x12", "-s", "-v", NULL },
                                        (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(srv != NULL, "cannot start the compositor");
    sleep_ms(1200);
    return srv;
}

static void test_comp_core(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    struct proc *srv = start_compositor();
    struct proc *cl = proc_create_user("/bin/comptest", (char *const[]){ "comptest", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start comptest");
    sleep_ms(1500);
    ktest_assert(pixel(40 + 100, 60 + 75) == 0x0000ff00, "green buffer on screen: %08x", pixel(140, 135));
    ktest_assert(pixel(40 + 10, 60 + 10) == 0x000000ff, "partial damage repainted: %08x", pixel(50, 70));
    int status = proc_reap(cl);
    ktest_assert(status == 0, "comptest status 0x%x", status);
    sleep_ms(200);
    ktest_assert(pixel(40 + 100, 60 + 75) == 0x00306080, "desktop after the surface was destroyed: %08x", pixel(140, 135));
    signal_send(srv, SIGTERM);
    status = proc_reap(srv);
    ktest_assert(status == 0, "compositor status 0x%x", status);
    kprintf("comp_core: compositor core ok\n");
}
KTEST_DEFINE("comp_core", test_comp_core);

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
    sleep_ms(1500);
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
 * queue fills, the server drops it after two seconds and keeps serving
 * a new client. */
static void test_gui_dead_client(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    int sw = logical_w(), sh = logical_h();
    struct proc *srv = start_server();
    struct proc *cl = proc_create_user("/bin/guitest", (char *const[]){ "guitest", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start guitest");
    sleep_ms(800);
    signal_send(cl, SIGKILL);
    proc_reap(cl);
    /* Flood the focused window with more messages than the queue holds. */
    int cx = sw / 2, cy = sh / 2;
    mouse_move_to(&cx, &cy, 70 + 100, 90 + 80, 0);
    for (int i = 0; i < 40; i++) {
        mouse_click(1);
        press_key(0x1e);
    }
    sleep_ms(2500);
    press_key(0x1e);
    sleep_ms(300);
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
    sleep_ms(1000);
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
    sleep_ms(50);
    mouse_move_to(&cx, &cy, 41 + 300 - 4, 59 + 200 - 4, 1);
    feed_packet(0, 0, 0);
    sleep_ms(500);
    ktest_assert(pixel(40 + 250, 60 + 175) == 0x00dcdcdc, "resized contents %08x", pixel(290, 235));
    /* Maximize button (second from the right, 14 px buttons 4 px
     * apart), then restore. */
    mouse_move_to(&cx, &cy, 41 + 300 - 27, 59 - 14, 0);
    mouse_click(1);
    sleep_ms(500);
    ktest_assert(pixel(sw - 10, sh - 10) == 0x00dcdcdc, "maximized contents %08x", pixel(sw - 10, sh - 10));
    mouse_move_to(&cx, &cy, sw - 28, 29 - 14, 0);
    mouse_click(1);
    sleep_ms(500);
    ktest_assert(pixel(sw - 10, sh - 10) == 0x00306080, "desktop after restore %08x", pixel(sw - 10, sh - 10));
    /* Move by the title bar: +60,+40. */
    mouse_move_to(&cx, &cy, 40 + 100, 59 - 12, 0);
    feed_packet(1, 0, 0);
    sleep_ms(50);
    mouse_move_to(&cx, &cy, 40 + 160, 59 - 12 + 40, 1);
    feed_packet(0, 0, 0);
    sleep_ms(300);
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
    sleep_ms(150);
    mouse_click(1);
    feed_packet_wheel(0, 0, 0, 1);
    sleep_ms(150);
    press_key(0x1e);                    /* a */
    ps2kbd_feed_scancode(0x2a);
    press_key(0x1e);                    /* A */
    ps2kbd_feed_scancode(0xaa);
    ps2kbd_feed_scancode(0xe0);
    ps2kbd_feed_scancode(0x48);
    ps2kbd_feed_scancode(0xe0);
    ps2kbd_feed_scancode(0xc8);         /* up */
    sleep_ms(300);
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
    char hold[8];
    if (cmdline_lookup("hold", hold, sizeof hold) && hold[0] == '1')
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
    sleep_ms(100);
    feed_packet(1, 0, 0);
    sleep_ms(400);
    mouse_move_to(&cx, &cy, 70 + 100, 90 + 75, 1);
    sleep_ms(300);
    feed_packet(0, 0, 0);
    int status = proc_reap(dst);
    ktest_assert(status == 0, "target status 0x%x", status);
    status = proc_reap(src);
    ktest_assert(status == 0, "source status 0x%x", status);
    signal_send(srv, SIGTERM);
    proc_reap(srv);
    kprintf("comp_data: data device ok\n");
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
    sleep_ms(1000);
    ktest_assert(pixel(sw / 2, sh - 14) == 0x0023272c, "panel drawn at the bottom: %08x", pixel(sw / 2, sh - 14));
    struct proc *cl = start_client("shell");
    ktest_assert(pixel(64 + 12 + 4, sh - 14) == 0x003f4854, "task button for the active window: %08x", pixel(80, sh - 14));
    int cx = sw / 2, cy = sh / 2;
    /* Minimize through the task button, restore through it. */
    mouse_move_to(&cx, &cy, 64 + 12 + 40, sh - 14, 0);
    mouse_click(1);
    sleep_ms(400);
    ktest_assert(pixel(40 + 100, 60 + 75) == 0x00306080, "window hidden after the task click: %08x", pixel(140, 135));
    mouse_click(1);
    sleep_ms(400);
    ktest_assert(pixel(40 + 100, 60 + 75) == 0x00dcdcdc, "window restored: %08x", pixel(140, 135));
    /* Launcher menu: open, dismiss it with a click on the desktop (the
     * compositor sends popup.done), then open it again and pick Clock.
     * The reopening once asked for a popup role on a surface that still
     * had one, a protocol error that disconnected the panel. */
    mouse_move_to(&cx, &cy, 30, sh - 14, 0);
    mouse_click(1);
    sleep_ms(400);
    mouse_move_to(&cx, &cy, sw - 100, 100, 0);
    mouse_click(1);
    sleep_ms(400);
    ktest_assert(pixel(sw / 2, sh - 14) == 0x0023272c, "panel alive after the dismissal: %08x", pixel(sw / 2, sh - 14));
    mouse_move_to(&cx, &cy, 30, sh - 14, 0);
    mouse_click(1);
    sleep_ms(400);
    mouse_move_to(&cx, &cy, 40, LAUNCHER_ROW(sh, LAUNCHER_CLOCK), 0);
    mouse_click(1);
    sleep_ms(1200);
    alt_key(0x3e);                      /* closes the clock, which is on top */
    sleep_ms(300);
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

/* A compositor that dies (here killed) must give the keyboard back to
 * the console: closing its descriptor drops the EVIOCGRAB grab. */
static void test_gui_kbd_restore(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    /* Pid 1 is treated as init and cannot be killed: use up that pid. */
    struct proc *first = proc_create_user("/bin/hello", (char *const[]){ "hello", NULL }, (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(first != NULL, "cannot start hello");
    proc_reap(first);
    struct proc *srv = proc_create_user("/bin/x12", (char *const[]){ "x12", "-s", NULL },
                                        (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(srv != NULL, "cannot start the compositor");
    sleep_ms(800);
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
        cl = proc_create_user(i == 3 ? "/home/.local/bin/hexview" : tools[i][0] == 'e' ? "/bin/evtest" : tools[i][0] == 's' ? "/bin/sysmon" : "/bin/logview",
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
    sleep_ms(1500);
    alt_key(0x3e);
    status = proc_reap(cl);
    ktest_assert(status == 0, "x12settings window status 0x%x", status);
    stop_server(srv);
    kprintf("gui_tools: debugging tools ok\n");
}
KTEST_DEFINE("gui_tools", test_gui_tools);

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
    struct proc *cl = proc_create_user("/home/.local/bin/unicode", (char *const[]){ "unicode", NULL },
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
    sleep_ms(400);
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
        int v = 0;
        while (*p >= '0' && *p <= '9')
            v = v * 10 + (*p++ - '0');
        *fields[i] = v;
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
    sleep_ms(500);
    struct proc *desktop = proc_create_user("/bin/desktop", (char *const[]){ "desktop", NULL }, (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(desktop != NULL, "cannot start the desktop");
    sleep_ms(2000);
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
    sleep_ms(1800);
    mid = pixel(sw / 2 + 100, sh / 2 + 50);
    ktest_assert(mid != 0x00306080, "wallpaper drawn in the middle of the screen: %08x", mid);
    int cx = sw / 2, cy = sh / 2;
    /* Double click on the first icon (Clock.app). */
    mouse_move_to(&cx, &cy, 55, 50, 0);
    mouse_click(1);
    mouse_click(1);
    sleep_ms(1500);
    ktest_assert(pixel(40 + 2, 60 - 10) == 0x00ebebeb, "clock window opened from the desktop: %08x", pixel(42, 50));
    alt_key(0x3e);
    sleep_ms(600);
    /* The desktop starts programs as children of init, so the clock is
     * not its zombie; here the kernel process adopted and holds it. */
    ktest_assert(zombies_of(desktop->pid) == 0, "the desktop left a zombie child");
    ktest_assert(zombies_of(0) == 1, "the clock became the kernel's orphan: %d", zombies_of(0));
    /* Context menus: on the desktop, then on the fourth icon (readme.txt). */
    mouse_move_to(&cx, &cy, 600, 300, 0);
    mouse_click(2);
    sleep_ms(300);
    mouse_move_to(&cx, &cy, 800, 500, 0);
    mouse_click(1);
    sleep_ms(300);
    mouse_move_to(&cx, &cy, 55, 300, 0);
    mouse_click(2);
    sleep_ms(300);
    mouse_move_to(&cx, &cy, 800, 500, 0);
    mouse_click(1);
    sleep_ms(300);
    /* Another setting from the command line. */
    cl = proc_create_user("/bin/settings", (char *const[]){ "settings", "set", "wallpaper_mode", "tile", NULL },
                          (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start settings");
    status = proc_reap(cl);
    ktest_assert(status == 0, "settings status 0x%x", status);
    sleep_ms(1800);
    /* The settings window (toplevel 2 cascades to 70,90). */
    cl = proc_create_user("/bin/settings", (char *const[]){ "settings", NULL }, (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start settings");
    sleep_ms(1800);
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
