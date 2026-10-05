/* The authentication dialog (docs/design/users.md). Settings runs as user,
 * a member of wheel, in a session of X12 for uid 1000. A change of the
 * time zone runs ln as root through sudo -A, and sudo runs askpass. The
 * dialog dims the screen. A wrong password shows the dialog again, and the
 * right one sets the zone. The time stamp of sudo lets the second change
 * pass without the dialog. After sudo -K the third change shows the dialog
 * again, and Escape cancels it. /etc/localtime then names the second
 * zone. */
#include <tests/ktest.h>
#include <console.h>
#include "gui_helpers.h"

#define TAB 0x0f
#define ESC 0x01

static int brightness(uint32_t c)
{
    return (int)(c >> 16 & 0xff) + (int)(c >> 8 & 0xff) + (int)(c & 0xff);
}

static void test_gui_askpass(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    run_shell("rm -f /etc/localtime /tmp/.askpass-*");
    run_shell("printf 'userpw\\nuserpw\\n' | passwd user");
    struct proc *srv = start_server();
    run_shell("x12settings set session_uid 1000");
    struct proc *cl = proc_create_user("/bin/doas", (char *const[]){ "doas", "-u", "user", "/bin/settings", "region", NULL },
                                       (char *const[]){ "PATH=/bin", NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start settings");
    ktest_wait_idle(2500);
    int x = logical_w() / 2, y = 8;
    int before = brightness(pixel(x, y));
    for (int i = 0; i < 3; i++) {                   /* the language, the formats, the time zone */
        press_key(TAB);
        sleep_ms(40);
    }
    press_down(1);                                  /* from UTC to Etc/UTC */
    ktest_assert(wait_procs("askpass", 1000, 1, 15000), "no authentication dialog");
    sleep_ms(1000);
    int dimmed = brightness(pixel(x, y));
    kprintf("gui_askpass: dimmed %d to %d\n", before, dimmed);
    ktest_assert(dimmed > 0 && dimmed * 4 < before * 3, "the screen is not dimmed (%d to %d)", before, dimmed);
    ktest_wait_idle(1500);                                 /* the screenshot of the qmp file */

    /* sudo starts the second dialog at once. The expect file finds its
     * surface in the log of X12. */
    type_line("wrongpw\n");
    sleep_ms(4000);
    ktest_assert(wait_procs("askpass", 1000, 1, 15000), "no second dialog after the wrong password");
    type_line("userpw\n");
    ktest_assert(wait_procs("askpass", 1000, 0, 5000), "the dialog remains after the right password");
    sleep_ms(3000);

    press_down(1);                                  /* Africa/Cairo, with the time stamp of sudo */
    ktest_wait_idle(3000);
    ktest_assert(count_procs("askpass", 1000) == 0, "a dialog despite the time stamp");

    run_shell("doas -u user /usr/bin/sudo -K");
    press_down(1);                                  /* Africa/Johannesburg, cancelled */
    ktest_assert(wait_procs("askpass", 1000, 1, 15000), "no dialog after sudo -K");
    press_key(ESC);
    ktest_assert(wait_procs("askpass", 1000, 0, 5000), "Escape did not close the dialog");
    ktest_wait_idle(2000);
    run_shell("echo link=$(readlink /etc/localtime)");

    alt_key(0x3e);
    int status = proc_reap(cl);
    ktest_assert(status == 0, "settings status 0x%x", status);
    run_shell("x12settings set session_uid -1");
    stop_server(srv);
    run_shell("rm -f /etc/localtime /home/user/.config/desktop.conf");
    kprintf("gui_askpass: ok\n");
}
KTEST_DEFINE("gui_askpass", test_gui_askpass);
