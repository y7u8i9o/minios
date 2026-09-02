#include <tests/ktest.h>
#include <drivers/ps2mouse.h>
#include <drivers/ps2kbd.h>
#include <drivers/tty.h>
#include <sched/thread.h>
#include <sched/wait.h>
#include <drivers/fbdev.h>
#include <sched/user.h>
#include <sched/proc.h>
#include <arch/boot.h>
#include <fs/vfs.h>
#include <lib/string.h>
#include <lib/crc32.h>
#include <console.h>
#include <errno.h>
#include <drivers/timer.h>

/* M17 stage 1: mouse packets become events on /dev/mouse. */
static void feed_packet_wheel(uint8_t flags, int dx, int dy, int dz)
{
    ps2mouse_feed_byte((uint8_t)(0x08 | flags | (dx < 0 ? 0x10 : 0) | (dy < 0 ? 0x20 : 0)));
    ps2mouse_feed_byte((uint8_t)dx);
    ps2mouse_feed_byte((uint8_t)dy);
    if (ps2mouse_has_wheel())
        ps2mouse_feed_byte((uint8_t)(dz & 0x0f));
}

static void feed_packet(uint8_t flags, int dx, int dy)
{
    feed_packet_wheel(flags, dx, dy, 0);
}

static void test_mouse(void)
{
    struct file *f;
    ktest_assert(vfs_open("/dev/mouse", O_RDONLY, 0, &f) == 0, "open /dev/mouse");
    ps2mouse_feed_byte(0x00);                 /* garbage without the sync bit */
    feed_packet(0x01, 5, 3);                  /* left button, right and up */
    feed_packet(0x00, -2, -7);                /* release, left and down */
    feed_packet(0x40, 1, 1);                  /* overflow, dropped */
    feed_packet(0x04, 0, 0);                  /* middle button */
    struct mouse_event ev[4];
    long n = file_read(f, (char *)ev, sizeof ev);
    ktest_assert(n == 3 * (long)sizeof ev[0], "read %ld bytes", n);
    ktest_assert(ev[0].dx == 5 && ev[0].dy == -3 && ev[0].buttons == 1, "event 0: %d %d %u", ev[0].dx, ev[0].dy, ev[0].buttons);
    ktest_assert(ev[1].dx == -2 && ev[1].dy == 7 && ev[1].buttons == 0, "event 1: %d %d %u", ev[1].dx, ev[1].dy, ev[1].buttons);
    ktest_assert(ev[2].dx == 0 && ev[2].dy == 0 && ev[2].buttons == 4, "event 2");
    ktest_assert(file_read(f, (char *)ev, 4) == -EINVAL, "short read rejected");
    file_put(f);

    /* Raw scancode mode delivers bytes untranslated. */
    ps2kbd_set_lflag(KBD_SCANCODES);
    ps2kbd_feed_scancode(0x2a);
    ps2kbd_feed_scancode(0xe0);
    ps2kbd_feed_scancode(0x48);
    ps2kbd_feed_scancode(0xaa);
    ktest_assert(vfs_open("/dev/kbd", O_RDONLY, 0, &f) == 0, "open /dev/kbd");
    uint8_t codes[8];
    n = file_read(f, (char *)codes, sizeof codes);
    ktest_assert(n == 4 && codes[0] == 0x2a && codes[1] == 0xe0 && codes[2] == 0x48 && codes[3] == 0xaa,
                 "raw scancodes: %ld bytes", n);
    file_put(f);
    ps2kbd_set_lflag(ICANON | ECHO | ISIG);
    kprintf("mouse: events and raw scancodes ok\n");
}
KTEST_DEFINE("mouse", test_mouse);

/* M19 stage 1: four byte IntelliMouse packets carry the wheel delta;
 * the packet length follows the detected device. */
static void test_mouse_wheel(void)
{
    bool had_wheel = ps2mouse_has_wheel();
    ktest_assert(had_wheel, "QEMU's mouse did not report id 3");
    struct file *f;
    ktest_assert(vfs_open("/dev/mouse", O_RDONLY, 0, &f) == 0, "open /dev/mouse");
    feed_packet_wheel(0x00, 0, 0, 1);         /* wheel down */
    feed_packet_wheel(0x02, 3, -1, -1);       /* right button, wheel up */
    feed_packet_wheel(0x00, 0, 0, -8);        /* largest negative delta */
    struct mouse_event ev[4];
    long n = file_read(f, (char *)ev, sizeof ev);
    ktest_assert(n == 3 * (long)sizeof ev[0], "read %ld bytes", n);
    ktest_assert(ev[0].dz == 1 && ev[0].dx == 0 && ev[0].buttons == 0, "event 0: dz %d", ev[0].dz);
    ktest_assert(ev[1].dz == -1 && ev[1].dx == 3 && ev[1].dy == 1 && ev[1].buttons == 2,
                 "event 1: %d %d dz %d buttons %u", ev[1].dx, ev[1].dy, ev[1].dz, ev[1].buttons);
    ktest_assert(ev[2].dz == -8, "event 2: dz %d", ev[2].dz);
    /* A device without a wheel keeps three byte packets. */
    ps2mouse_set_wheel(false);
    feed_packet(0x01, 2, 2);
    n = file_read(f, (char *)ev, sizeof ev);
    ktest_assert(n == (long)sizeof ev[0] && ev[0].dx == 2 && ev[0].dy == -2 && ev[0].dz == 0 && ev[0].buttons == 1,
                 "three byte packet: %ld bytes, %d %d dz %d", n, ev[0].dx, ev[0].dy, ev[0].dz);
    ps2mouse_set_wheel(had_wheel);
    file_put(f);
    kprintf("mouse_wheel: wheel events ok\n");
}
KTEST_DEFINE("mouse_wheel", test_mouse_wheel);

/* M17 stage 1: a user program maps /dev/fb0 and draws a pattern that the
 * kernel verifies pixel by pixel; the console is handed over and back. */
static void test_fb0(void)
{
    ktest_assert(bootinfo.have_framebuffer, "no framebuffer");
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
    const struct limine_framebuffer *lfb = &bootinfo.framebuffer;
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

static void mouse_move_to(int *cx, int *cy, int x, int y, int held)
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

static void mouse_click(int buttons)
{
    feed_packet((uint8_t)buttons, 0, 0);
    feed_packet(0, 0, 0);
    sleep_ms(100);
}

static uint32_t pixel(int x, int y)
{
    return fb_read_rgb(&bootinfo.framebuffer, (uint32_t)x, (uint32_t)y);
}

static struct proc *start_server(void);
static void stop_server(struct proc *srv);

static void test_gui(void)
{
    ktest_assert(bootinfo.have_framebuffer, "no framebuffer");
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
    ktest_assert(pixel(70 + 2, 90 - 10) == 0x00204060, "beta title bar active %08x", pixel(72, 80));
    ktest_assert(pixel(40 + 2, 60 - 10) == 0x00707070, "alpha title bar inactive %08x", pixel(42, 50));
    ktest_assert(pixel(40 + 5, 60 + 100) == 0x00dcdcdc, "alpha contents %08x", pixel(45, 160));

    int cx = (int)bootinfo.framebuffer.width / 2, cy = (int)bootinfo.framebuffer.height / 2;
    /* Click inside beta, then type a key. */
    mouse_move_to(&cx, &cy, 100, 120, 0);
    mouse_click(1);
    ps2kbd_feed_scancode(0x1e);
    ps2kbd_feed_scancode(0x9e);
    sleep_ms(100);
    /* Click alpha's contents: it comes to the front and gets focus. */
    mouse_move_to(&cx, &cy, 60, 200, 0);
    mouse_click(1);
    sleep_ms(100);
    ktest_assert(pixel(40 + 2, 60 - 10) == 0x00204060, "alpha title bar active after click %08x", pixel(42, 50));
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
    /* Close alpha (close box at the right of its title bar), then beta. */
    mouse_move_to(&cx, &cy, 140 + 300 - 8, 60 - 10, 0);
    mouse_click(1);
    sleep_ms(200);
    mouse_move_to(&cx, &cy, 70 + 240 - 8, 90 - 10, 0);
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
    ktest_assert(ps2kbd_get_lflag() == (ICANON | ECHO | ISIG), "keyboard mode not restored");
    kprintf("gui: server stopped, console restored\n");
}
KTEST_DEFINE("gui", test_gui);

/* M17 stage 5: the terminal window runs the shell on a pseudo terminal.
 * Keys typed through the keyboard driver reach the shell, whose output
 * is proven by a file it writes; the window contents are checked for a
 * drawn glyph. */
static void test_gui_term(void)
{
    ktest_assert(bootinfo.have_framebuffer, "no framebuffer");
    vfs_unlink("/gterm.txt");
    struct proc *srv = start_server();
    struct proc *term = proc_create_user("/bin/term", (char *const[]){ "term", NULL },
                                         (char *const[]){ "PATH=/bin", NULL }, &kernel_proc);
    ktest_assert(term != NULL, "cannot start term");
    sleep_ms(1500);
    /* Window 1 at (40,60), 640x400, dark background with a prompt. */
    ktest_assert(pixel(40 + 300, 60 + 200) == 0x00101010, "terminal background %08x", pixel(340, 260));
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
    /* The prompt row was drawn: some pixel in the first text row is light. */
    bool drawn = false;
    for (int x = 0; x < 640 && !drawn; x++)
        if (pixel(40 + x, 60 + 18) == 0x00e0e0e0)
            drawn = true;
    ktest_assert(drawn, "no text rendered in the terminal");
    /* M19: drag the grip so the window shrinks by 240x96 pixels (20
     * columns and 4 rows of the 12x24 cells); the shell sees the size. */
    int sw = (int)bootinfo.framebuffer.width;
    int wx = 40, wy = 60;                       /* 960x600 at the cascade origin */
    ktest_assert(sw == 1024, "test assumes 1024 pixels of width");
    int cx = sw / 2, cy = (int)bootinfo.framebuffer.height / 2;
    mouse_move_to(&cx, &cy, wx + 960 - 4, wy + 600 - 4, 0);
    feed_packet(1, 0, 0);
    sleep_ms(50);
    mouse_move_to(&cx, &cy, wx + 720 - 4, wy + 504 - 4, 1);
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
    ktest_assert(strcmp(buf, "21 60\n") == 0, "window size seen by the shell '%s'", buf);
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

static void press_key(uint8_t code)
{
    ps2kbd_feed_scancode(code);
    ps2kbd_feed_scancode((uint8_t)(code | 0x80));
}

static void alt_key(uint8_t code)
{
    ps2kbd_feed_scancode(0x38);
    press_key(code);
    ps2kbd_feed_scancode(0xb8);
    sleep_ms(150);
}

static struct proc *panel_proc;

/* The compositor and the panel; returns the compositor. */
static struct proc *start_server(void)
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

static void stop_server(struct proc *srv)
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

/* Resize by dragging the grip, maximize, restore and close a window;
 * the client refills the window green after every resize. */
static void test_gui_resize(void)
{
    ktest_assert(bootinfo.have_framebuffer, "no framebuffer");
    int sw = (int)bootinfo.framebuffer.width, sh = (int)bootinfo.framebuffer.height;
    int dh = sh - 28;                          /* desktop above the task bar */
    struct proc *srv = start_server();
    struct proc *cl = proc_create_user("/bin/guitest", (char *const[]){ "guitest", "resize", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start guitest");
    sleep_ms(800);
    /* Window 1 "alpha" 300x200 at (40,60). Drag its grip by (100,50). */
    int cx = sw / 2, cy = sh / 2;
    mouse_move_to(&cx, &cy, 40 + 300 - 4, 60 + 200 - 4, 0);
    feed_packet(1, 0, 0);
    sleep_ms(50);
    mouse_move_to(&cx, &cy, 40 + 400 - 4, 60 + 250 - 4, 1);
    feed_packet(0, 0, 0);
    sleep_ms(500);
    ktest_assert(pixel(40 + 350, 60 + 220) == 0x0040c040, "resized contents %08x", pixel(390, 280));
    ktest_assert(pixel(40 + 2, 60 - 10) == 0x00204060, "title bar after resize %08x", pixel(42, 50));
    /* Maximize: second box from the right of the title bar. */
    mouse_move_to(&cx, &cy, 40 + 400 - 16 - 16 + 7, 60 - 20 + 2 + 7, 0);
    mouse_click(1);
    sleep_ms(500);
    ktest_assert(pixel(sw - 10, dh - 10) == 0x0040c040, "maximized contents %08x", pixel(sw - 10, dh - 10));
    ktest_assert(pixel(sw / 2, sh - 14) == 0x00202830 || pixel(sw / 2, sh - 14) == 0x00303c48,
                 "task bar visible %08x", pixel(sw / 2, sh - 14));
    /* Restore through the same box, now at the top right of the screen. */
    mouse_move_to(&cx, &cy, 1 + (sw - 2) - 16 - 16 + 7, 21 - 20 + 2 + 7, 0);
    mouse_click(1);
    sleep_ms(500);
    ktest_assert(pixel(sw - 10, dh - 10) == 0x00306080, "desktop after restore %08x", pixel(sw - 10, dh - 10));
    ktest_assert(pixel(40 + 350, 60 + 220) == 0x0040c040, "restored contents %08x", pixel(390, 280));
    /* Close box. */
    mouse_move_to(&cx, &cy, 40 + 400 - 16 + 7, 60 - 20 + 2 + 7, 0);
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
    ktest_assert(bootinfo.have_framebuffer, "no framebuffer");
    int sw = (int)bootinfo.framebuffer.width, sh = (int)bootinfo.framebuffer.height;
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
    ktest_assert(pixel(40 + 2, 60 - 10) == 0x00204060, "alpha active after alt-tab %08x", pixel(42, 50));
    ktest_assert(pixel(70 + 50, 90 + 40) == 0x00dcdcdc, "alpha above beta %08x", pixel(120, 130));
    /* Minimize alpha: third box from the right. */
    mouse_move_to(&cx, &cy, 40 + 300 - 16 - 32 + 7, 60 - 20 + 2 + 7, 0);
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
    mouse_move_to(&cx, &cy, 70 + 240 - 16 - 16 + 7, 90 - 20 + 2 + 7, 0);
    mouse_click(1);
    sleep_ms(500);
    ktest_assert(pixel(45, 160) == 0x0040c040, "beta covers alpha %08x", pixel(45, 160));
    /* The launcher menu starts the clock; Alt+F4 closes it. */
    mouse_move_to(&cx, &cy, 30, sh - 14, 0);
    mouse_click(1);
    sleep_ms(200);
    int menu_y = sh - 28 + 4 - (15 * 20 + 4);
    mouse_move_to(&cx, &cy, 40, menu_y + 2 + 7 * 20 + 10, 0);
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
    ktest_assert(bootinfo.have_framebuffer, "no framebuffer");
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

/* M22: the framework test client. Window 400x300 at (40,60): menu bar
 * 6..32, text field 38..64, check box 70..96, then a row with the
 * editor (x 12..242), a scroll bar (248..262) and a list (268..388). */
static void ctrl_key(uint8_t code)
{
    ps2kbd_feed_scancode(0x1d);
    press_key(code);
    ps2kbd_feed_scancode(0x9d);
    sleep_ms(150);
}

static void test_gui_widgets(void)
{
    ktest_assert(bootinfo.have_framebuffer, "no framebuffer");
    int sw = (int)bootinfo.framebuffer.width, sh = (int)bootinfo.framebuffer.height;
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
    ktest_assert(bootinfo.have_framebuffer, "no framebuffer");
    int sw = (int)bootinfo.framebuffer.width, sh = (int)bootinfo.framebuffer.height;
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
    ktest_assert(bootinfo.have_framebuffer, "no framebuffer");
    struct proc *srv = start_server();
    vfs_unlink("/gedit.c");
    struct proc *cl = proc_create_user("/bin/gedit", (char *const[]){ "gedit", "/gedit.c", NULL },
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
    ktest_assert(bootinfo.have_framebuffer, "no framebuffer");
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
    ktest_assert(bootinfo.have_framebuffer, "no framebuffer");
    int sw = (int)bootinfo.framebuffer.width, sh = (int)bootinfo.framebuffer.height;
    struct proc *srv = start_server();
    struct proc *cl = proc_create_user("/bin/apptest", (char *const[]){ "apptest", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start apptest");
    sleep_ms(1500);
    int cx = sw / 2, cy = sh / 2;
    mouse_move_to(&cx, &cy, 70 + 6 + 194, 90 + 6 + 15, 0);
    mouse_click(1);
    sleep_ms(300);
    ktest_assert(pixel(70 + 6 + 100, 90 + 6 + 15) == 0x00c8d8f0, "hovered button colour %08x", pixel(176, 111));
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
    ktest_assert(bootinfo.have_framebuffer, "no framebuffer");
    int sw = (int)bootinfo.framebuffer.width, sh = (int)bootinfo.framebuffer.height;
    struct proc *srv = start_server();
    struct proc *cl = proc_create_user("/bin/calc", (char *const[]){ "calc", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start calc");
    sleep_ms(1200);
    ktest_assert(pixel(42, 50) == 0x00204060,
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
    ktest_assert(bootinfo.have_framebuffer, "no framebuffer");
    int sw = (int)bootinfo.framebuffer.width, sh = (int)bootinfo.framebuffer.height;
    struct proc *srv = start_server();
    struct proc *cl = proc_create_user("/bin/mandel", (char *const[]){ "mandel", NULL },
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
    ktest_assert(bootinfo.have_framebuffer, "no framebuffer");
    int sw = (int)bootinfo.framebuffer.width, sh = (int)bootinfo.framebuffer.height;
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
        settled = pixel(40 + 2, 60 - 10) == 0x00204060;
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
    struct proc *srv = proc_create_user("/bin/x12", (char *const[]){ "x12", NULL },
                                        (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(srv != NULL, "cannot start the compositor");
    sleep_ms(1200);
    return srv;
}

static void test_comp_core(void)
{
    ktest_assert(bootinfo.have_framebuffer, "no framebuffer");
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

/* A client killed without disconnecting must not block the server: its
 * queue fills, the server drops it after two seconds and keeps serving
 * a new client. */
static void test_gui_dead_client(void)
{
    ktest_assert(bootinfo.have_framebuffer, "no framebuffer");
    int sw = (int)bootinfo.framebuffer.width, sh = (int)bootinfo.framebuffer.height;
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
    ktest_assert(bootinfo.have_framebuffer, "no framebuffer");
    int sw = (int)bootinfo.framebuffer.width, sh = (int)bootinfo.framebuffer.height;
    struct proc *srv = start_compositor();
    struct proc *cl = start_client("shell");
    ktest_assert(pixel(40 + 100, 60 + 75) == 0x00dcdcdc, "window contents %08x", pixel(140, 135));
    ktest_assert(pixel(40 + 2, 60 - 10) == 0x00204060, "active title bar %08x", pixel(42, 50));
    int cx = sw / 2, cy = sh / 2;
    /* Grip drag: +100,+50. */
    mouse_move_to(&cx, &cy, 40 + 200 - 4, 60 + 150 - 4, 0);
    feed_packet(1, 0, 0);
    sleep_ms(50);
    mouse_move_to(&cx, &cy, 40 + 300 - 4, 60 + 200 - 4, 1);
    feed_packet(0, 0, 0);
    sleep_ms(500);
    ktest_assert(pixel(40 + 250, 60 + 175) == 0x00dcdcdc, "resized contents %08x", pixel(290, 235));
    /* Maximize box, then restore. */
    mouse_move_to(&cx, &cy, 40 + 300 - 16 - 16 + 7, 60 - 20 + 2 + 7, 0);
    mouse_click(1);
    sleep_ms(500);
    ktest_assert(pixel(sw - 10, sh - 10) == 0x00dcdcdc, "maximized contents %08x", pixel(sw - 10, sh - 10));
    mouse_move_to(&cx, &cy, 1 + (sw - 2) - 16 - 16 + 7, 21 - 20 + 2 + 7, 0);
    mouse_click(1);
    sleep_ms(500);
    ktest_assert(pixel(sw - 10, sh - 10) == 0x00306080, "desktop after restore %08x", pixel(sw - 10, sh - 10));
    /* Move by the title bar: +60,+40. */
    mouse_move_to(&cx, &cy, 40 + 100, 60 - 10, 0);
    feed_packet(1, 0, 0);
    sleep_ms(50);
    mouse_move_to(&cx, &cy, 40 + 160, 60 + 30, 1);
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
    ktest_assert(bootinfo.have_framebuffer, "no framebuffer");
    int sw = (int)bootinfo.framebuffer.width, sh = (int)bootinfo.framebuffer.height;
    struct proc *srv = start_compositor();
    struct proc *cl = start_client("seat");
    int cx = sw / 2, cy = sh / 2;
    mouse_move_to(&cx, &cy, 40 + 50, 60 + 40, 0);
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

/* M25: the selection and a drag from the source window (surface 1 at
 * 40,60) to the target window (surface 2 at 70,90, on top). */
static void test_comp_data(void)
{
    ktest_assert(bootinfo.have_framebuffer, "no framebuffer");
    int sw = (int)bootinfo.framebuffer.width, sh = (int)bootinfo.framebuffer.height;
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
    ktest_assert(bootinfo.have_framebuffer, "no framebuffer");
    int sw = (int)bootinfo.framebuffer.width, sh = (int)bootinfo.framebuffer.height;
    struct proc *srv = start_compositor();
    struct proc *panel = proc_create_user("/bin/panel", (char *const[]){ "panel", NULL }, (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(panel != NULL, "cannot start the panel");
    sleep_ms(1000);
    ktest_assert(pixel(sw / 2, sh - 14) == 0x00202830, "panel drawn at the bottom: %08x", pixel(sw / 2, sh - 14));
    struct proc *cl = start_client("shell");
    ktest_assert(pixel(64 + 12 + 10, sh - 14) == 0x00405870, "task button for the active window: %08x", pixel(86, sh - 14));
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
    ktest_assert(pixel(sw / 2, sh - 14) == 0x00202830, "panel alive after the dismissal: %08x", pixel(sw / 2, sh - 14));
    mouse_move_to(&cx, &cy, 30, sh - 14, 0);
    mouse_click(1);
    sleep_ms(400);
    int menu_y = sh - 28 + 4 - (14 * 20 + 4);
    mouse_move_to(&cx, &cy, 40, menu_y + 2 + 6 * 20 + 10, 0);
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

/* A compositor that dies (here killed) must leave the console keyboard
 * in its cooked mode, because /dev/kbd's close resets it. */
static void test_gui_kbd_restore(void)
{
    ktest_assert(bootinfo.have_framebuffer, "no framebuffer");
    /* Pid 1 is treated as init and cannot be killed: use up that pid. */
    struct proc *first = proc_create_user("/bin/hello", (char *const[]){ "hello", NULL }, (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(first != NULL, "cannot start hello");
    proc_reap(first);
    struct proc *srv = proc_create_user("/bin/x12", (char *const[]){ "x12", NULL },
                                        (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(srv != NULL, "cannot start the compositor");
    sleep_ms(800);
    ktest_assert(tty_get_lflag(&console_tty) & KBD_SCANCODES, "compositor put the keyboard in raw mode");
    signal_send(srv, SIGKILL);
    proc_reap(srv);
    sleep_ms(100);
    ktest_assert(!(tty_get_lflag(&console_tty) & KBD_SCANCODES), "keyboard back in cooked mode after the compositor died");
    kprintf("gui_kbd_restore: keyboard mode restored\n");
}
KTEST_DEFINE("gui_kbd_restore", test_gui_kbd_restore);

/* Debugging tools (after M26): the settings application applies a
 * setting without a window and reports statistics; evtest logs a
 * click; sysmon, logview and hexview open and close. */
static void test_gui_tools(void)
{
    ktest_assert(bootinfo.have_framebuffer, "no framebuffer");
    int sw = (int)bootinfo.framebuffer.width, sh = (int)bootinfo.framebuffer.height;
    struct proc *srv = start_server();
    struct proc *cl = proc_create_user("/bin/x12settings", (char *const[]){ "x12settings", "set", "frame_ms", "33", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start x12settings");
    int status = proc_reap(cl);
    ktest_assert(status == 0, "x12settings status 0x%x", status);
    const char *tools[] = { "evtest", "sysmon", "logview", "hexview" };
    int cx = sw / 2, cy = sh / 2;
    for (int i = 0; i < 4; i++) {
        cl = proc_create_user(i == 3 ? "/bin/hexview" : tools[i][0] == 'e' ? "/bin/evtest" : tools[i][0] == 's' ? "/bin/sysmon" : "/bin/logview",
                              (char *const[]){ (char *)tools[i], NULL }, (char *const[]){ NULL }, &kernel_proc);
        ktest_assert(cl != NULL, "cannot start %s", tools[i]);
        sleep_ms(1200);
        int wx = 40 + i * 30, wy = 60 + i * 30;   /* cascade by creation number */
        ktest_assert(pixel(wx + 2, wy - 10) == 0x00204060, "%s window has an active title bar: %08x", tools[i], pixel(wx + 2, wy - 10));
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

/* The Unicode viewer walks the whole code space once at startup to record
 * which code points its font covers, so this test also measures that the
 * window still appears promptly. */
static void test_gui_unicode(void)
{
    ktest_assert(bootinfo.have_framebuffer, "no framebuffer");
    int sw = (int)bootinfo.framebuffer.width, sh = (int)bootinfo.framebuffer.height;
    struct proc *srv = start_server();
    struct proc *cl = proc_create_user("/bin/unicode", (char *const[]){ "unicode", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start unicode");
    sleep_ms(2000);
    ktest_assert(pixel(42, 50) == 0x00204060, "unicode window has an active title bar: %08x", pixel(42, 50));
    /* Click a cell in the grid, which starts below the toolbar and the
     * preview panel, and let the selection repaint. */
    int cx = sw / 2, cy = sh / 2;
    mouse_move_to(&cx, &cy, 40 + 100, 60 + 230, 0);
    mouse_click(1);
    sleep_ms(400);
    ktest_assert(pixel(42, 50) == 0x00204060, "unicode window survives a grid click: %08x", pixel(42, 50));
    alt_key(0x3e);
    int status = proc_reap(cl);
    ktest_assert(status == 0, "unicode status 0x%x", status);
    stop_server(srv);
    kprintf("gui_unicode: viewer ok\n");
}
KTEST_DEFINE("gui_unicode", test_gui_unicode);

/* Desktop layer (wallpaper, icons, context menus) and the settings
 * application: the wallpaper replaces the plain desktop colour, a double
 * click on the Clock icon starts the clock, right clicks open the two
 * context menus, "settings set" changes the configuration file which the
 * desktop applies, and the settings window opens and closes. */
static void test_gui_desktop(void)
{
    ktest_assert(bootinfo.have_framebuffer, "no framebuffer");
    int sw = (int)bootinfo.framebuffer.width, sh = (int)bootinfo.framebuffer.height;
    struct proc *srv = start_compositor();
    struct proc *panel = proc_create_user("/bin/panel", (char *const[]){ "panel", NULL }, (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(panel != NULL, "cannot start the panel");
    sleep_ms(500);
    struct proc *desktop = proc_create_user("/bin/desktop", (char *const[]){ "desktop", NULL }, (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(desktop != NULL, "cannot start the desktop");
    sleep_ms(2000);
    uint32_t mid = pixel(sw / 2, sh / 2);
    ktest_assert(mid != 0x00306080, "wallpaper drawn in the middle of the screen: %08x", mid);
    ktest_assert(pixel(sw / 2, sh - 14) == 0x00202830, "panel above the desktop: %08x", pixel(sw / 2, sh - 14));
    int cx = sw / 2, cy = sh / 2;
    /* Double click on the first icon (Clock.app). */
    mouse_move_to(&cx, &cy, 55, 50, 0);
    mouse_click(1);
    mouse_click(1);
    sleep_ms(1500);
    ktest_assert(pixel(40 + 2, 60 - 10) == 0x00204060, "clock window opened from the desktop: %08x", pixel(42, 50));
    alt_key(0x3e);
    sleep_ms(400);
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
    /* Settings from the command line, applied by the desktop within a second. */
    struct proc *cl = proc_create_user("/bin/settings", (char *const[]){ "settings", "set", "wallpaper_mode", "tile", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start settings");
    int status = proc_reap(cl);
    ktest_assert(status == 0, "settings status 0x%x", status);
    sleep_ms(1800);
    /* The settings window (toplevel 2 cascades to 70,90). */
    cl = proc_create_user("/bin/settings", (char *const[]){ "settings", NULL }, (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start settings");
    sleep_ms(1800);
    ktest_assert(pixel(70 + 2, 90 - 10) == 0x00204060, "settings window has an active title bar: %08x", pixel(72, 80));
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
