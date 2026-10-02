/* L7: the Region and language page of Settings.  The keyboard selects the
 * French language and the zone Asia/Tokyo on the page.  The configuration
 * file then names the language, /etc/localtime names the zone, and a
 * program started through /etc/profile shows its French title. */
#include <tests/ktest.h>
#include <drivers/ps2kbd.h>
#include <drivers/fbdev.h>
#include <drivers/timer.h>
#include <sched/user.h>
#include <sched/proc.h>
#include <lib/string.h>
#include <console.h>
#include "gui_helpers.h"

static char *const env[] = { "PATH=/bin", "HOME=/home", NULL };

static void tap(uint8_t code)
{
    ps2kbd_feed_scancode(code);
    ps2kbd_feed_scancode((uint8_t)(code | 0x80));
    sleep_ms(40);
}

static void taps(uint8_t code, int n)
{
    while (n-- > 0)
        tap(code);
}

#define TAB  0x0f
#define DOWN 0x50                   /* 0xe0 0x50 */

static void down(int n)
{
    while (n-- > 0) {
        ps2kbd_feed_scancode(0xe0);
        ps2kbd_feed_scancode(DOWN);
        ps2kbd_feed_scancode(0xe0);
        ps2kbd_feed_scancode(DOWN | 0x80);
        sleep_ms(40);
    }
}

static void sh(const char *command)
{
    struct proc *p = proc_create_user("/bin/sh", (char *const[]){ "sh", "-c", (char *)command, NULL }, env, &kernel_proc);
    ktest_assert(p != NULL, "cannot start sh");
    int status = proc_reap(p);
    ktest_assert(status == 0, "'%s' status 0x%x", command, status);
}

static void test_gui_region(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    sh("rm -f /home/.config/desktop.conf /etc/localtime");
    struct proc *srv = start_server();
    struct proc *cl = proc_create_user("/bin/settings", (char *const[]){ "settings", "region", NULL }, env, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start settings");
    sleep_ms(2000);
    tap(TAB);                       /* the language */
    down(2);                        /* en_US, es_ES, fr_FR */
    taps(TAB, 2);                   /* the formats, the time zone */
    down(33);                       /* from UTC to Asia/Tokyo in zones.tab */
    sleep_ms(300);
    alt_key(0x3e);
    int status = proc_reap(cl);
    ktest_assert(status == 0, "settings status 0x%x", status);

    sh("grep lang= /home/.config/desktop.conf");
    sh("echo zone=$(date +%Z)");
    cl = proc_create_user("/bin/sh", (char *const[]){ "sh", "-c", ". /etc/profile; sysmon", NULL }, env,
                          &kernel_proc);
    ktest_assert(cl != NULL, "cannot start sysmon");
    sleep_ms(2000);
    alt_key(0x3e);
    status = proc_reap(cl);
    ktest_assert(status == 0, "sysmon status 0x%x", status);
    stop_server(srv);
    sh("rm -f /home/.config/desktop.conf /etc/localtime");
    kprintf("gui_region: ok\n");
}
KTEST_DEFINE("gui_region", test_gui_region);
