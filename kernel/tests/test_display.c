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
#include "gui_helpers.h"

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
    ktest_assert(device_pixel(10, 10) == 0x00336699, "fill colour %08x", device_pixel(10, 10));
    ktest_assert(device_pixel(101, 100) == 0x00ff8800 && device_pixel(100, 100) == 0x000044ff, "pattern %08x %08x",
                 device_pixel(100, 100), device_pixel(101, 100));
    ktest_assert(device_pixel(1279, 799) == 0x00336699, "last pixel of the new mode %08x", device_pixel(1279, 799));
    kprintf("gpu_mode: user mode change and flush ok\n");
    /* The qmp script of the case takes a screendump after the line of
     * fbmodetest about the flushed squares. */
    sleep_ms(1500);
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
    struct proc *srv = start_x12((char *const[]){ "x12", "-s", NULL });
    /* The first frame of X12 draws the desktop and the cursor. */
    ktest_wait_idle(1200);
    int sw = (int)fb_screen.width, sh = (int)fb_screen.height;
    /* x = ax * sw / 32768, so ax = x * 32768 / sw lands exactly. */
    int x = 300, y = 200;
    virtio_input_feed(EV_ABS, ABS_X, (uint32_t)(x * 32768 / sw));
    virtio_input_feed(EV_ABS, ABS_Y, (uint32_t)((y * 32768 + sh - 1) / sh));
    virtio_input_feed(EV_SYN, SYN_REPORT, 0);
    ktest_wait_idle(300);
    ktest_assert(arrow_cursor_at(x, y), "the cursor is not at %d,%d", x, y);
    ktest_assert(device_pixel(sw / 2, sh / 2) == 0x00306080, "old cursor position repainted: %08x",
                 device_pixel(sw / 2, sh / 2));
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
    struct proc *srv = start_x12((char *const[]){ "x12", "-s", NULL });
    /* The first frame of X12 draws the desktop and the cursor. */
    ktest_wait_idle(1200);
    ktest_assert(device_pixel(0, 0) == 0x00306080, "desktop pixel %08x", device_pixel(0, 0));
    struct proc *cl = proc_create_user("/bin/guitest", (char *const[]){ "guitest", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start guitest");
    ktest_wait_idle(1200);
    /* Window alpha 300x200 at (40,60), beta 240x160 at (70,90), beta
     * focused. guitest draws device pixels: beta's red rectangle is at
     * device 20..119 x 20..79 inside the surface. */
    ktest_assert(device_pixel(70 * S + 60, 90 * S + 40) == 0x00ff0000, "beta red rect at device coordinates: %08x",
                 device_pixel(70 * S + 60, 90 * S + 40));
    /* alpha's 8x8 device checkerboard at surface device (20,40). */
    int cx = 40 * S + 20, cy = 60 * S + 40;
    ktest_assert(device_pixel(cx, cy) == 0x00000000 && device_pixel(cx + 1, cy) == 0x00ffffff &&
                     device_pixel(cx, cy + 1) == 0x00ffffff && device_pixel(cx + 1, cy + 1) == 0x00000000,
                 "checkerboard copied 1:1: %08x %08x %08x", device_pixel(cx, cy), device_pixel(cx + 1, cy),
                 device_pixel(cx, cy + 1));
    /* Client decorations at the scale: beta's 30 px header bar is
     * 30 * S device rows tall, its last S rows the hairline, sampled at
     * the middle of the window (the corners are rounded); above it the
     * outline blends over the shadow. */
    int top = (90 - 30) * S, mid = (70 + 120) * S;
    ktest_assert(device_pixel(mid, top) == 0x00ebebeb && device_pixel(mid, top + S - 1) == 0x00ebebeb,
                 "header bar starts at the scaled row: %08x %08x", device_pixel(mid, top),
                 device_pixel(mid, top + S - 1));
    ktest_assert(device_pixel(mid, 90 * S - S - 1) == 0x00ebebeb, "header bar spans the scaled height: %08x",
                 device_pixel(mid, 90 * S - S - 1));
    ktest_assert(device_pixel(mid, 90 * S - 1) == 0x00d4d4d4, "hairline row above the contents: %08x",
                 device_pixel(mid, 90 * S - 1));
    ktest_assert(device_pixel(mid, top - 1) != 0x00ebebeb, "above the frame is the outline: %08x",
                 device_pixel(mid, top - 1));
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
    struct proc *srv = start_x12((char *const[]){ "x12", "-s", NULL });
    struct proc *panel = run("/bin/panel", (char *const[]){ "panel", NULL });
    ktest_wait_idle(500);
    struct proc *desktop = run("/bin/desktop", (char *const[]){ "desktop", NULL });
    ktest_wait_idle(800);
    struct proc *term = run("/bin/term", (char *const[]){ "term", NULL });
    ktest_wait_idle(1500);
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
    ktest_assert(device_pixel(S * (sw / 2), S * (sh - 14)) == 0x0023272c, "panel after the changes: %08x",
                 device_pixel(S * (sw / 2), S * (sh - 14)));
    ktest_assert(device_pixel(S * 42, S * 50) == 0x00ebebeb || device_pixel(S * 42, S * 50) == 0x00fafafa,
                 "terminal title bar after the changes: %08x", device_pixel(S * 42, S * 50));
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

/* V3 of docs/plan/release-0.6.0.md: the host display requests sizes
 * through the VNC message SetDesktopSize (tests/cases/gpu_resize/qmp). The
 * compositor follows the first two requests. The third request arrives
 * after the setting display_follow was turned off and leaves the mode
 * unchanged. */
static void wait_request(uint32_t serial)
{
    struct fb_display d;
    for (int i = 0; i < 300; i++) {
        fb_display_get(&d);
        if (d.serial >= serial)
            return;
        sleep_ms(100);
    }
    ktest_assert(false, "no display request %u after 30 s", serial);
}

static void wait_mode(uint32_t width, uint32_t height)
{
    for (int i = 0; i < 100 && (fb_screen.width != width || fb_screen.height != height); i++)
        sleep_ms(100);
    ktest_assert(fb_screen.width == width && fb_screen.height == height, "mode %lux%lu instead of %ux%u",
                 fb_screen.width, fb_screen.height, width, height);
    /* The panel and the desktop draw again at the new width. */
    int sw = logical_w(), sh = logical_h();
    uint32_t left = 0, right = 0;
    for (int i = 0; i < 100; i++) {
        left = device_pixel(1, sh - 2);
        right = device_pixel(sw - 2, sh - 2);
        if (left == 0x0023272c && right == 0x0023272c)
            break;
        sleep_ms(100);
    }
    ktest_assert(left == 0x0023272c && right == 0x0023272c, "panel from %08x to %08x at %dx%d", left, right, sw, sh);
    /* The screendump of the qmp script reads the display of the host. */
    sleep_ms(500);
    kprintf("gpu_resize: mode %ux%u, the panel spans %d pixels\n", width, height, sw);
}

static void test_gpu_resize(void)
{
    ktest_assert(fb_screen_present && fb_has_gpu(), "needs virtio-gpu");
    struct proc *srv = run("/bin/x12", (char *const[]){ "x12", "-s", NULL });
    ktest_wait_idle(1200);
    struct proc *panel = run("/bin/panel", (char *const[]){ "panel", NULL });
    ktest_wait_idle(500);
    struct proc *desktop = run("/bin/desktop", (char *const[]){ "desktop", NULL });
    ktest_wait_idle(800);
    struct fb_display d;
    fb_display_get(&d);
    uint32_t serial = d.serial;
    kprintf("gpu_resize: session ready at %lux%lu\n", fb_screen.width, fb_screen.height);
    wait_request(serial + 1);
    wait_mode(1600, 1000);
    wait_request(serial + 2);
    wait_mode(1280, 720);
    struct proc *p = run("/bin/settings", (char *const[]){ "settings", "set", "display_follow", "0", NULL });
    ktest_assert(proc_reap(p) == 0, "settings set display_follow 0 failed");
    sleep_ms(1000);
    kprintf("gpu_resize: the mode no longer follows the window\n");
    wait_request(serial + 3);
    sleep_ms(2000);
    fb_display_get(&d);
    ktest_assert(d.width == 1440 && d.height == 900, "third request %ux%u", d.width, d.height);
    ktest_assert(fb_screen.width == 1280 && fb_screen.height == 720, "mode %lux%lu after a request without follow",
                 fb_screen.width, fb_screen.height);
    kprintf("gpu_resize: request 1440x900 ignored, mode 1280x720\n");
    signal_send(desktop, SIGTERM);
    proc_reap(desktop);
    signal_send(panel, SIGTERM);
    proc_reap(panel);
    signal_send(srv, SIGTERM);
    ktest_assert(proc_reap(srv) == 0, "compositor status");
}
KTEST_DEFINE("gpu_resize", test_gpu_resize);

/* B1 of docs/plan/desktop-panel.md: the desktop draws the wallpaper at the
 * device resolution with filtering. The screen is 2560x1600 at scale 2.
 * A wallpaper of 2560x1600 pixels in vertical stripes of one pixel must
 * appear pixel for pixel. A wallpaper of 2x2 pixels stretched to the
 * screen must show interpolated colours between its pixels. */
static void set_setting(const char *key, const char *value)
{
    struct proc *p = run("/bin/settings", (char *const[]){ "settings", "set", (char *)key, (char *)value, NULL });
    ktest_assert(proc_reap(p) == 0, "settings set %s %s failed", key, value);
}

static bool stripes_shown(void)
{
    uint32_t a = device_pixel(1000, 800), b = device_pixel(1001, 800);
    return (a == 0x00000000 && b == 0x00ffffff) || (a == 0x00ffffff && b == 0x00000000);
}

static void test_wallpaper_hidpi(void)
{
    ktest_assert(fb_screen.width == 2560 && fb_screen_scale == 2, "mode %lux%lu scale %u", fb_screen.width,
                 fb_screen.height, fb_screen_scale);
    struct proc *srv = run("/bin/x12", (char *const[]){ "x12", "-s", NULL });
    ktest_wait_idle(1200);
    struct proc *desktop = run("/bin/desktop", (char *const[]){ "desktop", NULL });
    ktest_wait_idle(800);
    set_setting("wallpaper_mode", "fill");
    set_setting("wallpaper", "/etc/tests/wallpaper-stripes.png");
    for (int i = 0; i < 100 && !stripes_shown(); i++)
        sleep_ms(100);
    ktest_assert(stripes_shown(), "stripes of one device pixel: %08x %08x", device_pixel(1000, 800),
                 device_pixel(1001, 800));
    kprintf("wallpaper_hidpi: stripes of one device pixel at 1000,800: %06x %06x\n", device_pixel(1000, 800),
            device_pixel(1001, 800));

    set_setting("wallpaper_mode", "stretch");
    set_setting("wallpaper", "/etc/tests/wallpaper-2x2.png");
    uint32_t c = 0;
    for (int i = 0; i < 100; i++) {
        c = device_pixel(1280, 400);
        if (c != 0x00000000 && c != 0x00ffffff)
            break;
        sleep_ms(100);
    }
    sleep_ms(500);
    c = device_pixel(1280, 400);
    uint32_t r = c >> 16 & 0xff, g = c >> 8 & 0xff, b = c & 0xff;
    ktest_assert(r >= 0x60 && r <= 0xa0 && g >= 0x60 && g <= 0xa0 && b < 0x20,
                 "between the red and the green pixel: %06x", c);
    uint32_t corner = device_pixel(2, 2);
    ktest_assert(corner == 0x00ff0000, "the top left corner is red: %06x", corner);
    kprintf("wallpaper_hidpi: stretched 2x2 image, %06x between red and green\n", c);
    /* The default wallpaper for the screendump of the QMP script. */
    set_setting("wallpaper_mode", "fill");
    set_setting("wallpaper", "/usr/share/wallpapers/default.png");
    for (int i = 0; i < 100 && device_pixel(2, 2) == 0x00ff0000; i++)
        sleep_ms(100);
    sleep_ms(1000);
    kprintf("wallpaper_hidpi: default wallpaper drawn\n");
    sleep_ms(3000);
    signal_send(desktop, SIGTERM);
    proc_reap(desktop);
    signal_send(srv, SIGTERM);
    ktest_assert(proc_reap(srv) == 0, "compositor status");
}
KTEST_DEFINE("wallpaper_hidpi", test_wallpaper_hidpi);
