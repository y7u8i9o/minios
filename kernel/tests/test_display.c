/* M32: virtio-gpu mode changes and virtio-input tablet events. */
#include <tests/ktest.h>
#include <drivers/fbdev.h>
#include <drivers/fbcon.h>
#include <drivers/virtio/virtio_gpu.h>
#include <drivers/virtio/virtio_input.h>
#include <drivers/timer.h>
#include <mm/pmm.h>
#include <drivers/ps2kbd.h>
#include <input/input.h>
#include <lib/printf.h>
#include <fs/vfs.h>
#include <sched/proc.h>
#include <sched/user.h>
#include <ipc/signal.h>
#include <lib/string.h>
#include <console.h>
#include <errno.h>

static void test_console_sgr(void)
{
    ktest_assert(fbcon_present(), "no framebuffer console");
    const char *text = "\033[2J\033[H\033[1;31mX\033[0mY\033[44;93;7mZ\033[27;22;39;49mW";
    console_write(text, strlen(text));
    console_flush();
    char c; uint8_t attr;
    ktest_assert(fbcon_get_cell(0, 0, &c, &attr) && c == 'X' && attr == 9, "bright red: %u", attr);
    ktest_assert(fbcon_get_cell(1, 0, &c, &attr) && c == 'Y' && attr == 7, "reset: %u", attr);
    ktest_assert(fbcon_get_cell(2, 0, &c, &attr) && c == 'Z' && attr == 0xb4, "reverse: %u", attr);
    ktest_assert(fbcon_get_cell(3, 0, &c, &attr) && c == 'W' && attr == 7, "defaults: %u", attr);
    text = "\033[H\n\033[1A\033[K";
    console_write(text, strlen(text));
    console_flush();
    ktest_assert(fbcon_get_cell(0, 0, &c, &attr) && c == ' ' && attr == 7, "erase attributes");
    kprintf("console_sgr: ok\n");
}
KTEST_DEFINE("console_sgr", test_console_sgr);

static uint32_t pixel(int x, int y)
{
    return fb_read_rgb(&fb_screen, (uint32_t)x, (uint32_t)y);
}

static int logical_w(void) { return (int)(fb_screen.width / (fb_screen_scale ? fb_screen_scale : 1)); }
static int logical_h(void) { return (int)(fb_screen.height / (fb_screen_scale ? fb_screen_scale : 1)); }

/* The GPU driver took over at boot; the console follows a mode change
 * done from the kernel; fbmodetest changes the mode through the ioctl,
 * draws and flushes. */
static void test_gpu_mode(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    ktest_assert(virtio_gpu_present() && fb_has_gpu(), "virtio-gpu not registered");
    ktest_assert(fb_screen.bpp == 32 && fb_screen.red_mask_shift == 16, "gpu buffer layout");
    ktest_assert(fb_map_size() == 16u << 20, "map size %zu", fb_map_size());
    uint16_t cols, rows;
    fbcon_get_size(&cols, &rows);
    ktest_assert(cols == 128 && rows == 48, "console %ux%u cells at boot", cols, rows);

    ktest_assert(fb_set_mode(1600, 1200, 2) == 0, "set 1600x1200@2");
    ktest_assert(fb_screen.width == 1600 && fb_screen.height == 1200 && fb_screen.pitch == 6400,
                 "geometry after the change: %lux%lu pitch %lu", fb_screen.width, fb_screen.height, fb_screen.pitch);
    ktest_assert(fb_screen_scale == 2, "scale %u", fb_screen_scale);
    fbcon_get_size(&cols, &rows);
    ktest_assert(cols == 100 && rows == 37, "console %ux%u cells at 1600x1200@2", cols, rows);
    kprintf("gpu_mode: console follows the mode\n");
    ktest_assert(fb_set_mode(8192, 8192, 1) == -EINVAL, "oversized mode refused");
    ktest_assert(fb_set_mode(1024, 768, 1) == 0, "back to 1024x768");

    vfs_unlink("/fb.ready");
    struct proc *p = proc_create_user("/bin/fbmodetest", (char *const[]){ "fbmodetest", NULL },
                                      (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(p != NULL, "cannot start /bin/fbmodetest");
    struct inode *marker = NULL;
    for (int i = 0; i < 200 && vfs_lookup("/fb.ready", &marker) < 0; i++)
        sleep_ms(50);
    ktest_assert(marker != NULL, "fbmodetest did not signal readiness");
    inode_put(marker);
    ktest_assert(fb_screen.width == 1280 && fb_screen.height == 800, "mode set through the ioctl: %lux%lu",
                 fb_screen.width, fb_screen.height);
    ktest_assert(pixel(10, 10) == 0x00336699, "fill colour %08x", pixel(10, 10));
    ktest_assert(pixel(101, 100) == 0x00ff8800 && pixel(100, 100) == 0x000044ff, "pattern %08x %08x",
                 pixel(100, 100), pixel(101, 100));
    ktest_assert(pixel(1279, 799) == 0x00336699, "last pixel of the new mode %08x", pixel(1279, 799));
    kprintf("gpu_mode: user mode change and flush ok\n");
    ps2kbd_feed_scancode(0x1c);
    ps2kbd_feed_scancode(0x9c);
    int status = proc_reap(p);
    ktest_assert(status == 0, "fbmodetest status 0x%x", status);
    ktest_assert(fb_screen.width == 1024 && fb_screen.height == 768, "mode restored by fbmodetest");
    fbcon_get_size(&cols, &rows);
    ktest_assert(cols == 128 && rows == 48, "console restored %ux%u", cols, rows);
    kprintf("gpu_mode: ok\n");
}
KTEST_DEFINE("gpu_mode", test_gpu_mode);

/* Absolute events, buttons and the wheel of the attached tablet reach
 * its /dev/input node unchanged; the axis ranges come from the device. */
static void test_input_tablet(void)
{
    struct input_dev *d = input_device_by_name("QEMU Virtio Tablet");
    ktest_assert(d != NULL, "no virtio tablet device");
    ktest_assert(d->abs[ABS_X].maximum == 32767 && d->abs[ABS_Y].maximum == 32767, "axis range %d, %d",
                 d->abs[ABS_X].maximum, d->abs[ABS_Y].maximum);
    char path[32];
    ksnprintf(path, sizeof path, "/dev/input/event%u", d->index);
    struct file *f;
    ktest_assert(vfs_open(path, O_RDONLY, 0, &f) == 0, "open %s", path);
    virtio_input_feed(EV_ABS, ABS_X, 16384);
    virtio_input_feed(EV_ABS, ABS_Y, 8192);
    virtio_input_feed(EV_KEY, BTN_LEFT, 1);
    virtio_input_feed(EV_SYN, SYN_REPORT, 0);
    virtio_input_feed(EV_REL, REL_WHEEL, (uint32_t)-1);
    virtio_input_feed(EV_SYN, SYN_REPORT, 0);
    virtio_input_feed(EV_KEY, BTN_LEFT, 0);
    virtio_input_feed(EV_KEY, BTN_RIGHT, 1);
    virtio_input_feed(EV_SYN, SYN_REPORT, 0);
    virtio_input_feed(EV_SYN, SYN_REPORT, 0);      /* nothing changed: no event */
    struct input_event ev[16];
    long n = file_read(f, (char *)ev, sizeof ev);
    ktest_assert(n == 9 * (long)sizeof ev[0], "read %ld bytes", n);
    static const struct { uint16_t type, code; int32_t value; } want[9] = {
        { EV_ABS, ABS_X, 16384 }, { EV_ABS, ABS_Y, 8192 }, { EV_KEY, BTN_LEFT, 1 }, { EV_SYN, SYN_REPORT, 0 },
        { EV_REL, REL_WHEEL, -1 }, { EV_SYN, SYN_REPORT, 0 },
        { EV_KEY, BTN_LEFT, 0 }, { EV_KEY, BTN_RIGHT, 1 }, { EV_SYN, SYN_REPORT, 0 },
    };
    for (int i = 0; i < 9; i++)
        ktest_assert(ev[i].type == want[i].type && ev[i].code == want[i].code && ev[i].value == want[i].value,
                     "event %d: %u %u %d", i, ev[i].type, ev[i].code, ev[i].value);
    file_put(f);
    kprintf("input_tablet: absolute events ok\n");
}
KTEST_DEFINE("input_tablet", test_input_tablet);

/* The compositor places the cursor from absolute events. */
static void test_gui_tablet(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    struct proc *srv = proc_create_user("/bin/x12", (char *const[]){ "x12", "-s", NULL }, (char *const[]){ NULL },
                                        &kernel_proc);
    ktest_assert(srv != NULL, "cannot start the compositor");
    sleep_ms(1200);
    int sw = (int)fb_screen.width, sh = (int)fb_screen.height;
    /* x = ax * sw / 32768, so ax = x * 32768 / sw lands exactly. */
    int x = 300, y = 200;
    virtio_input_feed(EV_ABS, ABS_X, (uint32_t)(x * 32768 / sw));
    virtio_input_feed(EV_ABS, ABS_Y, (uint32_t)((y * 32768 + sh - 1) / sh));
    virtio_input_feed(EV_SYN, SYN_REPORT, 0);
    sleep_ms(300);
    ktest_assert(pixel(x, y) == 0x00000000 && pixel(x + 1, y + 2) == 0x00ffffff,
                 "cursor at %d,%d: %08x %08x", x, y, pixel(x, y), pixel(x + 1, y + 2));
    ktest_assert(pixel(sw / 2, sh / 2) == 0x00306080, "old cursor position repainted: %08x", pixel(sw / 2, sh / 2));
    kprintf("gui_tablet: cursor follows absolute events\n");
    signal_send(srv, SIGTERM);
    int status = proc_reap(srv);
    ktest_assert(status == 0, "compositor status 0x%x", status);
}
KTEST_DEFINE("gui_tablet", test_gui_tablet);

/* M33: with an output scale of 2, libgui clients render at twice the
 * resolution into buffers with buffer scale 2, which the compositor
 * copies 1:1; decorations are drawn at the scale. */
static void test_gui_scale2(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    ktest_assert(fb_screen_scale == 2, "scale %u, expected 2 (video=WxH@2)", fb_screen_scale);
    int S = 2;
    struct proc *srv = proc_create_user("/bin/x12", (char *const[]){ "x12", "-s", NULL }, (char *const[]){ NULL },
                                        &kernel_proc);
    ktest_assert(srv != NULL, "cannot start the compositor");
    sleep_ms(1200);
    ktest_assert(pixel(0, 0) == 0x00306080, "desktop pixel %08x", pixel(0, 0));
    struct proc *cl = proc_create_user("/bin/guitest", (char *const[]){ "guitest", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start guitest");
    sleep_ms(1200);
    /* Window alpha 300x200 at (40,60), beta 240x160 at (70,90), beta
     * focused. guitest draws device pixels: beta's red rectangle is at
     * device 20..119 x 20..79 inside the surface. */
    ktest_assert(pixel(70 * S + 60, 90 * S + 40) == 0x00ff0000, "beta red rect at device coordinates: %08x",
                 pixel(70 * S + 60, 90 * S + 40));
    /* alpha's 8x8 device checkerboard at surface device (20,40). */
    int cx = 40 * S + 20, cy = 60 * S + 40;
    ktest_assert(pixel(cx, cy) == 0x00000000 && pixel(cx + 1, cy) == 0x00ffffff && pixel(cx, cy + 1) == 0x00ffffff &&
                 pixel(cx + 1, cy + 1) == 0x00000000, "checkerboard copied 1:1: %08x %08x %08x", pixel(cx, cy),
                 pixel(cx + 1, cy), pixel(cx, cy + 1));
    /* Client decorations at the scale: beta's 30 px header bar is
     * 30 * S device rows tall, its last S rows the hairline, sampled at
     * the middle of the window (the corners are rounded); above it the
     * outline blends over the shadow. */
    int top = (90 - 30) * S, mid = (70 + 120) * S;
    ktest_assert(pixel(mid, top) == 0x00ebebeb && pixel(mid, top + S - 1) == 0x00ebebeb,
                 "header bar starts at the scaled row: %08x %08x", pixel(mid, top), pixel(mid, top + S - 1));
    ktest_assert(pixel(mid, 90 * S - S - 1) == 0x00ebebeb, "header bar spans the scaled height: %08x",
                 pixel(mid, 90 * S - S - 1));
    ktest_assert(pixel(mid, 90 * S - 1) == 0x00d4d4d4, "hairline row above the contents: %08x", pixel(mid, 90 * S - 1));
    ktest_assert(pixel(mid, top - 1) != 0x00ebebeb, "above the frame is the outline: %08x", pixel(mid, top - 1));
    kprintf("gui_scale2: clients render at scale %d\n", S);
    signal_send(cl, SIGTERM);
    proc_reap(cl);
    signal_send(srv, SIGTERM);
    int status = proc_reap(srv);
    ktest_assert(status == 0, "compositor status 0x%x", status);
}
KTEST_DEFINE("gui_scale2", test_gui_scale2);

/* Mode changes at run time with a session running: the desktop applies
 * display_mode from the configuration file, every client re-creates its
 * buffers at the new scale, and memory comes back. */
static struct proc *run(const char *path, char *const argv[])
{
    struct proc *p = proc_create_user(path, argv, (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(p != NULL, "cannot start %s", path);
    return p;
}

static void set_mode(const char *mode)
{
    struct proc *p = run("/bin/settings", (char *const[]){ "settings", "set", "display_mode", (char *)mode, NULL });
    ktest_assert(proc_reap(p) == 0, "settings set display_mode %s failed", mode);
    sleep_ms(2500);
}

static void test_gui_modes(void)
{
    ktest_assert(fb_screen_present && fb_has_gpu(), "needs virtio-gpu");
    struct proc *srv = run("/bin/x12", (char *const[]){ "x12", "-s", NULL });
    sleep_ms(1200);
    struct proc *panel = run("/bin/panel", (char *const[]){ "panel", NULL });
    sleep_ms(500);
    struct proc *desktop = run("/bin/desktop", (char *const[]){ "desktop", NULL });
    sleep_ms(800);
    struct proc *term = run("/bin/term", (char *const[]){ "term", NULL });
    sleep_ms(1500);
    static const char *const modes[] = { "1024x768@1", "2560x1600@2", "1024x768@1", "2560x1600@2", "1920x1200@1", "2560x1600@2" };
    struct pmm_stats st;
    pmm_get_stats(&st);
    uint64_t free0 = st.free_pages;
    for (unsigned i = 0; i < sizeof modes / sizeof modes[0]; i++) {
        set_mode(modes[i]);
        pmm_get_stats(&st);
        uint32_t s = fb_screen_scale ? fb_screen_scale : 1;
        kprintf("gui_modes: %s -> %lux%lu scale %u, %lu free pages (%ld since start)\n", modes[i], fb_screen.width,
                fb_screen.height, s, (unsigned long)st.free_pages, (long)st.free_pages - (long)free0);
    }
    ktest_assert(fb_screen.width == 2560 && fb_screen_scale == 2, "final mode %lux%lu scale %u", fb_screen.width,
                 fb_screen.height, fb_screen_scale);
    /* The panel is at the bottom, the terminal has a title bar. */
    int sw = logical_w(), sh = logical_h();
    uint32_t S = fb_screen_scale;
    ktest_assert(pixel(S * (sw / 2), S * (sh - 14)) == 0x0023272c, "panel after the changes: %08x",
                 pixel(S * (sw / 2), S * (sh - 14)));
    ktest_assert(pixel(S * 42, S * 50) == 0x00ebebeb || pixel(S * 42, S * 50) == 0x00fafafa,
                 "terminal title bar after the changes: %08x", pixel(S * 42, S * 50));
    kprintf("gui_modes: session survived the mode changes\n");
    signal_send(term, SIGTERM);
    proc_reap(term);
    signal_send(desktop, SIGTERM);
    proc_reap(desktop);
    signal_send(panel, SIGTERM);
    proc_reap(panel);
    signal_send(srv, SIGTERM);
    ktest_assert(proc_reap(srv) == 0, "compositor status");
}
KTEST_DEFINE("gui_modes", test_gui_modes);
