#pragma once
/* Helpers shared by the GUI boot tests (test_gui.c, test_audio_gui.c):
 * synthetic mouse and keyboard input, logical pixels of the composed
 * screen and the desktop session processes. */
#include <tests/ktest.h>
#include <drivers/ps2mouse.h>
#include <drivers/ps2kbd.h>
#include <drivers/virtio/virtio_input.h>
#include <drivers/fbdev.h>
#include <drivers/timer.h>
#include <sched/user.h>
#include <sched/proc.h>
#include <ipc/signal.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <lib/crc32.h>
#include <fs/vfs.h>
#include <console.h>

static struct proc *panel_proc;

/* The geometry and the colours of the panel in logical pixels, as
 * user/panel/panel.h defines them. sw is the logical width of the screen. */
#define PANEL_H 28
#define PANEL_ROW(sh) ((sh) - PANEL_H / 2)      /* the middle row of the bar */
#define PANEL_MENU_X 30                         /* inside the Menu button */
#define PANEL_TASKS_X (4 + 76 + 8)              /* the first window button */
#define PANEL_TASK_W 160
#define PANEL_CLOCK_W 128
#define PANEL_MIXER_W 30
#define PANEL_NOTIFY_W 30
#define PANEL_INPUT_W 30
#define PANEL_DESKTOP_W 24
#define PANEL_DESKTOP_X(sw) ((sw) - PANEL_DESKTOP_W)
#define PANEL_POWER_W 30
#define PANEL_POWER_X(sw) (PANEL_DESKTOP_X(sw) - 4 - PANEL_POWER_W)
#define PANEL_CLOCK_X(sw) (PANEL_POWER_X(sw) - PANEL_CLOCK_W)
#define PANEL_NOTIFY_X(sw) (PANEL_CLOCK_X(sw) - PANEL_NOTIFY_W - 4)
#define PANEL_MIXER_X(sw) (PANEL_NOTIFY_X(sw) - PANEL_MIXER_W - 4)
#define PANEL_INPUT_X(sw) (PANEL_MIXER_X(sw) - PANEL_INPUT_W - 4)
#define PANEL_BG 0x0023272c
#define PANEL_BUTTON_ACTIVE 0x003f4854
#define PANEL_BUTTON_HOVER 0x00384049

/* GUI tests install the same archive a user installs on the base image. */
static inline void install_app(const char *name)
{
    struct proc *p = proc_create_user("/bin/sh",
        (char *const[]){ "sh", "/etc/tests/install-app.sh", (char *)name, NULL },
        (char *const[]){ "PATH=/bin", NULL }, &kernel_proc);
    ktest_assert(p != NULL, "cannot start pkg for %s", name);
    int status = proc_reap(p);
    ktest_assert(status == 0, "install %s status 0x%x", name, status);
}

/* PS/2 mouse packets: buttons and the wheel (the drivers report them
 * to the input core, the compositor reads /dev/input). */
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

static inline int logical_w(void);
static inline int logical_h(void);

/* Place the cursor at a logical position through the tablet (the
 * attached virtio tablet, or the virtual one): absolute events are
 * exact, relative motion of the PS/2 mouse is accelerated by the
 * compositor. pressed is unused: the buttons pressed through PS/2 packets
 * remain pressed across the move. */
static inline void mouse_move_to(int *cx, int *cy, int x, int y, int pressed)
{
    (void)pressed;
    /* The compositor maps ax to floor(ax * width / 32768). */
    int w = logical_w(), h = logical_h();
    virtio_input_feed(EV_ABS, ABS_X, (uint32_t)((x * (VIRTIO_INPUT_ABS_MAX + 1) + w - 1) / w));
    virtio_input_feed(EV_ABS, ABS_Y, (uint32_t)((y * (VIRTIO_INPUT_ABS_MAX + 1) + h - 1) / h));
    virtio_input_feed(EV_SYN, SYN_REPORT, 0);
    *cx = x;
    *cy = y;
    ktest_wait_idle(30);
}

static inline void mouse_click(int buttons)
{
    feed_packet((uint8_t)buttons, 0, 0);
    feed_packet(0, 0, 0);
    ktest_wait_idle(100);
}

/* A logical pixel of the composed screen: the GUI tests describe the
 * desktop in logical coordinates and run at any pixel scale. */
static inline uint32_t pixel(int x, int y)
{
    uint32_t s = fb_screen_scale ? fb_screen_scale : 1;
    return fb_read_rgb(&fb_screen, (uint32_t)x * s, (uint32_t)y * s);
}

/* True when the arrow cursor has its tip at the logical pixel (x, y): a
 * black tip, and white fill one pixel right and two pixels down. The
 * framebuffer does not contain a device cursor (FB_CAP_CURSOR, G9 of
 * docs/plan/compositor-performance.md). For a device cursor the check
 * uses the position and the image that /dev/fb0 recorded. A failed check
 * prints what it found. */
static inline bool arrow_cursor_at(int x, int y)
{
    int32_t s = fb_screen_scale ? (int32_t)fb_screen_scale : 1;
    struct fb_cursor_state *c = kmalloc(sizeof *c);
    ktest_assert(c != NULL, "alloc");
    fb_cursor_get(c);
    bool ok;
    if (c->visible) {
        ok = c->x == x * s && c->y == y * s && c->hot_x == 0 && c->hot_y == 0 && c->image[0] == 0xff000000u &&
             c->image[2 * s * FB_CURSOR_MAX + s] == 0xffffffffu;
        if (!ok)
            kprintf("device cursor at %d,%d, hotspot %u,%u, pixels %08x %08x\n", c->x, c->y, c->hot_x, c->hot_y,
                    c->image[0], c->image[2 * s * FB_CURSOR_MAX + s]);
    } else {
        ok = pixel(x, y) == 0x00000000 && pixel(x + 1, y + 2) == 0x00ffffff;
        if (!ok)
            kprintf("framebuffer at %d,%d: %08x %08x\n", x, y, pixel(x, y), pixel(x + 1, y + 2));
    }
    kfree(c);
    return ok;
}

/* The CRC-32 of the file at path and its size in bytes. */
static inline uint32_t file_crc32(const char *path, long *size)
{
    struct file *f;
    ktest_assert(vfs_open(path, O_RDONLY, 0, &f) == 0, "open %s", path);
    char *buf = kmalloc(4096);
    ktest_assert(buf != NULL, "alloc");
    uint32_t crc = 0;
    long total = 0, got;
    while ((got = file_read(f, buf, 4096)) > 0) {
        crc = crc32(crc, buf, (size_t)got);
        total += got;
    }
    kfree(buf);
    file_put(f);
    *size = total;
    return crc;
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

/* Press the down arrow n times. It is an extended key of set 1. */
static inline void press_down(int n)
{
    while (n-- > 0) {
        ps2kbd_feed_scancode(0xe0);
        ps2kbd_feed_scancode(0x50);
        ps2kbd_feed_scancode(0xe0);
        ps2kbd_feed_scancode(0xd0);
        ktest_wait_idle(40);
    }
}

/* Run a shell command as root, with the path /bin and the home /home, and
 * return its exit status. */
static inline int shell_status(const char *command)
{
    struct proc *p = proc_create_user("/bin/sh", (char *const[]){ "sh", "-c", (char *)command, NULL },
                                      (char *const[]){ "PATH=/bin", "HOME=/home", NULL }, &kernel_proc);
    ktest_assert(p != NULL, "cannot start sh");
    return proc_reap(p);
}

/* Run a shell command as shell_status does. The command must exit with
 * status 0. */
static inline void run_shell(const char *command)
{
    int status = shell_status(command);
    ktest_assert(status == 0, "'%s' status 0x%x", command, status);
}

/* Run a shell command every 100 ms until it exits with status 0, at most
 * ms milliseconds. Returns true when it did. */
static inline bool wait_shell(const char *command, int ms)
{
    for (int waited = 0; waited < ms; waited += 100) {
        if (shell_status(command) == 0)
            return true;
        sleep_ms(100);
    }
    return false;
}

static inline void alt_key(uint8_t code)
{
    ps2kbd_feed_scancode(0x38);
    press_key(code);
    ps2kbd_feed_scancode(0xb8);
    ktest_wait_idle(150);
}

/* The compositor and the panel; returns the compositor. */
static inline struct proc *start_server(void)
{
    struct proc *srv = proc_create_user("/bin/x12", (char *const[]){ "x12", "-s", NULL },
                                        (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(srv != NULL, "cannot start the compositor");
    ktest_wait_idle(600);
    panel_proc = proc_create_user("/bin/panel", (char *const[]){ "panel", NULL }, (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(panel_proc != NULL, "cannot start the panel");
    ktest_wait_idle(600);
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
    ktest_wait_idle(150);
}

/* The number of live processes named name with effective uid uid, or of
 * any uid when uid is -1, from the table of /dev/proc. The CPU time in
 * milliseconds and the resident size in KiB of the first such process go
 * to ticks and rss_kb when these are not NULL, and remain unchanged when
 * no process matches. */
static inline int proc_table_find(const char *name, int uid, unsigned long *ticks, unsigned long *rss_kb)
{
    size_t size = 8192;
    char *table = kmalloc(size);
    ktest_assert(table != NULL, "alloc");
    proc_format_table(table, size);
    int n = 0;
    for (char *line = table; *line;) {
        char *end = strchr(line, '\n');
        if (end)
            *end = '\0';
        /* PID PPID PGID STATE TIME RSS UID NAME */
        char *f[8];
        int nf = 0;
        for (char *p = line; *p && nf < 8;) {
            while (*p == ' ')
                p++;
            if (!*p)
                break;
            f[nf++] = p;
            while (*p && *p != ' ')
                p++;
            if (*p)
                *p++ = '\0';
        }
        if (nf == 8 && strcmp(f[3], "zombie") != 0 && strcmp(f[7], name) == 0) {
            int u = (int)strtoull(f[6], NULL, 10);
            if (uid < 0 || u == uid) {
                if (n == 0 && ticks)
                    *ticks = (unsigned long)strtoull(f[4], NULL, 10);
                if (n == 0 && rss_kb)
                    *rss_kb = (unsigned long)strtoull(f[5], NULL, 10);
                n++;
            }
        }
        if (!end)
            break;
        line = end + 1;
    }
    kfree(table);
    return n;
}

static inline int count_procs(const char *name, int uid)
{
    return proc_table_find(name, uid, NULL, NULL);
}

/* Wait until the count is unchanged for half a second. The greeter forks its
 * helpers, which carry its name until they exec, so a single look could
 * count one of them. */
static inline bool wait_procs(const char *name, int uid, int want, int ms)
{
    int steady = 0;
    for (int t = 0; t < ms; t += 100) {
        steady = count_procs(name, uid) == want ? steady + 1 : 0;
        if (steady == 5)
            return true;
        sleep_ms(100);
    }
    return false;
}

/* Log in at the login window of the greeter as the preselected account
 * user (uid 1000), which has no password: Enter chooses the account, a
 * second Enter logs in with the empty password, and the greeter asks for
 * a new password twice before the session starts. Waits for the panel and
 * the desktop of the session. */
static inline void greeter_login_user(void)
{
    press_key(0x1c);
    sleep_ms(800);
    press_key(0x1c);
    sleep_ms(1500);
    type_line("userpw\n");
    sleep_ms(300);
    type_line("userpw\n");
    ktest_assert(wait_procs("panel", 1000, 1, 10000), "no panel of uid 1000");
    ktest_assert(wait_procs("desktop", 1000, 1, 5000), "no desktop of uid 1000");
}

/* The power menu of the panel (user/panel/power.c): 200 pixels wide, three
 * rows of 28 pixels with 6 pixels of padding, above the power button and
 * aligned to its right edge. Row 0 is Log out, 1 Restart, 2 Shut down. */
#define POWER_MENU_W 200
#define POWER_MENU_H (2 * 6 + 3 * 28)
static inline void panel_power_choose(int sw, int sh, int row)
{
    int cx = sw / 2, cy = sh / 2;
    mouse_move_to(&cx, &cy, PANEL_POWER_X(sw) + PANEL_POWER_W / 2, PANEL_ROW(sh), 0);
    mouse_click(1);
    sleep_ms(600);
    int x0 = PANEL_POWER_X(sw) + PANEL_POWER_W - POWER_MENU_W, y0 = sh - PANEL_H + 4 - POWER_MENU_H;
    mouse_move_to(&cx, &cy, x0 + 60, y0 + 6 + row * 28 + 14, 0);
    mouse_click(1);
}
