/* L7: the Region and language page of Settings.  The keyboard selects the
 * French language and the zone Asia/Tokyo on the page.  The configuration
 * file then names the language, /etc/localtime names the zone and
 * /etc/profile exports the language.  The running panel and desktop
 * switch to French.  The panel draws the icons of the launcher, whose SVG
 * numbers do not follow the decimal comma of French, and starts sysmon
 * with its French title.  The desktop rebuilds its menus. */
#include <tests/ktest.h>
#include <drivers/ps2kbd.h>
#include <drivers/fbdev.h>
#include <drivers/timer.h>
#include <sched/user.h>
#include <sched/proc.h>
#include <lib/string.h>
#include <console.h>
#include <ipc/signal.h>
#include "gui_helpers.h"

static char *const env[] = { "PATH=/bin", "HOME=/home", NULL };

static void tap(uint8_t code)
{
    ps2kbd_feed_scancode(code);
    ps2kbd_feed_scancode((uint8_t)(code | 0x80));
    ktest_wait_idle(40);
}

/* Set 1 scancodes of the letters a to z. */
static const uint8_t letter_code[26] = {
    0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
    0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c,
};

static void type(const char *s)
{
    for (; *s; s++)
        tap(letter_code[*s - 'a']);
}

/* icon_pixels counts the dark grey pixels in the column of the launcher
 * icons, between the left edge of the menu and its titles.  The menu is
 * light and the desktop is blue. */
static int icon_pixels(void)
{
    int n = 0, bottom = logical_h() - 28;
    for (int y = 0; y < bottom; y++)
        for (int x = 16; x < 36; x++) {
            uint32_t c = pixel(x, y);
            int r = (int)(c >> 16 & 0xff), g = (int)(c >> 8 & 0xff), b = (int)(c & 0xff);
            int hi = r > g ? (r > b ? r : b) : (g > b ? g : b), lo = r < g ? (r < b ? r : b) : (g < b ? g : b);
            n += hi < 0x90 && hi - lo < 24;
        }
    return n;
}

static void taps(uint8_t code, int n)
{
    while (n-- > 0)
        tap(code);
}

#define TAB  0x0f

static void test_gui_region(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    run_shell("rm -f /home/.config/desktop.conf /etc/localtime");
    /* The panel reads the language from HOME and starts sysmon through
     * PATH. The test therefore starts the compositor alone and then the
     * panel with the environment env. */
    struct proc *srv = start_x12((char *const[]){ "x12", "-s", NULL });
    struct proc *panel = proc_create_user("/bin/panel", (char *const[]){ "panel", NULL }, env, &kernel_proc);
    ktest_assert(panel != NULL, "cannot start the panel");
    struct proc *desktop = proc_create_user("/bin/desktop", (char *const[]){ "desktop", NULL }, env, &kernel_proc);
    ktest_assert(desktop != NULL, "cannot start the desktop");
    ktest_wait_idle(1500);
    struct proc *cl = proc_create_user("/bin/settings", (char *const[]){ "settings", "region", NULL }, env, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start settings");
    ktest_wait_idle(2000);
    tap(TAB);                       /* the language */
    press_down(2);                  /* en_US, es_ES, fr_FR */
    taps(TAB, 2);                   /* the formats, the time zone */
    /* From UTC to Asia/Tokyo in zones.tab. The last two presses follow
     * each other without a pause, so the last one arrives while ln
     * applies the zone of the one before. The page must still apply
     * Asia/Tokyo at the end. */
    press_down(31);
    for (int i = 0; i < 2; i++) {
        ps2kbd_feed_scancode(0xe0);
        ps2kbd_feed_scancode(0x50);
        ps2kbd_feed_scancode(0xe0);
        ps2kbd_feed_scancode(0xd0);
    }
    ktest_wait_idle(1000);
    alt_key(0x3e);
    int status = proc_reap(cl);
    ktest_assert(status == 0, "settings status 0x%x", status);

    run_shell("grep lang= /home/.config/desktop.conf");
    run_shell("echo zone=$(date +%Z)");
    run_shell(". /etc/profile; echo profile LANG=$LANG");

    /* The panel has read the new language within a second.  The launcher
     * finds sysmon by its French title and starts it. */
    int cx = logical_w() / 2, cy = logical_h() / 2;
    mouse_move_to(&cx, &cy, 30, logical_h() - 14, 0);
    mouse_click(1);
    sleep_ms(1500);
    int icons = icon_pixels();
    kprintf("gui_region: %d icon pixels\n", icons);
    ktest_assert(icons > 200, "the launcher draws no icons (%d pixels)", icons);
    type("moni");
    ktest_wait_idle(300);
    tap(0x1c);
    ktest_wait_idle(2000);
    alt_key(0x3e);
    ktest_wait_idle(500);
    /* The context menu of the desktop, built again in French. */
    mouse_move_to(&cx, &cy, logical_w() / 2, logical_h() / 2, 0);
    mouse_click(2);
    ktest_wait_idle(800);
    tap(0x01);
    ktest_wait_idle(300);
    signal_send(desktop, SIGTERM);
    status = proc_reap(desktop);
    ktest_assert((status & 0x7f) == SIGTERM, "desktop status 0x%x", status);
    signal_send(panel, SIGTERM);
    proc_reap(panel);
    stop_server(srv);
    run_shell("rm -f /home/.config/desktop.conf /etc/localtime");
    kprintf("gui_region: ok\n");
}
KTEST_DEFINE("gui_region", test_gui_region);
