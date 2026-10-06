/* S5 of docs/plan/release-0.7.0.md: notifications on the desktop. The
 * test starts notifyd, X12 and the panel. It posts notifications with
 * notify-send and operates their pop-ups and the history with the mouse.
 *
 * A pop-up card is 340 pixels wide. It lies 8 pixels from the top and
 * right edges of the desktop area, and the panel is at the bottom. A card
 * with one body line and actions is 10 + 18 + 18 + 18 + 30 + 10 pixels
 * high. Its first action button begins 10 pixels from the left edge of the
 * card. */
#include <tests/ktest.h>
#include <drivers/fbdev.h>
#include <drivers/timer.h>
#include <sched/user.h>
#include <sched/proc.h>
#include <ipc/signal.h>
#include <console.h>
#include <fs/vfs.h>
#include "gui_helpers.h"

#define CARD_W 340
#define CARD_BG 0x00fafbfc
#define HIST_W 340
#define HIST_H(rows) (2 * 6 + 32 + (rows) * 46 + 32)

static struct proc *start_program(const char *path, const char *name)
{
    struct proc *p = proc_create_user(path, (char *const[]){ (char *)name, NULL },
                                      (char *const[]){ "PATH=/bin", "HOME=/home", NULL }, &kernel_proc);
    ktest_assert(p != NULL, "cannot start %s", name);
    return p;
}

static void test_gui_notify(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    int sw = logical_w(), sh = logical_h();
    struct proc *notifyd = start_program("/bin/notifyd", "notifyd");
    ktest_wait_idle(300);
    struct proc *srv = start_server();
    /* The panel connects to notifyd when it starts. */
    ktest_wait_idle(1500);
    int x0 = sw - 8 - CARD_W, y0 = 8;
    int cx = sw / 2, cy = sh / 2;

    /* A notification with two actions. notify-send waits for the result. */
    struct proc *ns = proc_create_user("/bin/sh",
        (char *const[]){ "sh", "-c",
                         "notify-send -a Mail -A open=Open -A later=Later -w 'New message' "
                         "'From Ada: lunch at noon?' > /tmp/notify.out", NULL },
        (char *const[]){ "PATH=/bin", "HOME=/home", NULL }, &kernel_proc);
    ktest_assert(ns != NULL, "cannot start notify-send");
    ktest_wait_idle(1500);
    ktest_assert(pixel(x0 + 4, y0 + 4) == CARD_BG, "pop-up at the top right: %08x", pixel(x0 + 4, y0 + 4));
    kprintf("gui_notify: pop-up shown\n");
    sleep_ms(1500);                     /* the screendump of the QMP script */

    /* The first action button: "Open". */
    int by = y0 + 10 + 3 * 18 + 3;
    mouse_move_to(&cx, &cy, x0 + 10 + 8, by + 12, 0);
    mouse_click(1);
    ktest_wait_idle(800);
    int status = proc_reap(ns);
    ktest_assert(status == 0, "notify-send status 0x%x", status);
    ktest_assert(shell_status("grep -q '^action open$' /tmp/notify.out") == 0, "notify-send reported the action");
    ktest_assert(shell_status("grep -q '^closed dismissed$' /tmp/notify.out") == 0, "notify-send reported the close");
    ktest_assert(pixel(x0 + 4, y0 + 4) != CARD_BG, "pop-up closed after the action: %08x", pixel(x0 + 4, y0 + 4));
    kprintf("gui_notify: action invoked\n");

    /* A pop-up with a short timeout disappears without user input. */
    run_shell("notify-send -t 800 'Short' 'This one expires'");
    ktest_wait_idle(400);
    ktest_assert(pixel(x0 + 4, y0 + 4) == CARD_BG, "short pop-up shown: %08x", pixel(x0 + 4, y0 + 4));
    sleep_ms(1500);
    ktest_wait_idle(400);
    ktest_assert(pixel(x0 + 4, y0 + 4) != CARD_BG, "short pop-up expired: %08x", pixel(x0 + 4, y0 + 4));
    kprintf("gui_notify: pop-up expired\n");

    /* The close button in the top right corner of the card dismisses it. */
    run_shell("notify-send -t 0 'Persistent' 'Closed with the button'");
    ktest_wait_idle(600);
    ktest_assert(pixel(x0 + 4, y0 + 4) == CARD_BG, "persistent pop-up shown: %08x", pixel(x0 + 4, y0 + 4));
    mouse_move_to(&cx, &cy, x0 + CARD_W - 10 - 10, y0 + 10 - 1 + 10, 0);
    mouse_click(1);
    ktest_wait_idle(600);
    ktest_assert(pixel(x0 + 4, y0 + 4) != CARD_BG, "pop-up dismissed: %08x", pixel(x0 + 4, y0 + 4));
    kprintf("gui_notify: pop-up dismissed\n");

    /* The bell opens the history with the three notifications. The popup
     * appears above the button and is right-aligned with it. */
    mouse_move_to(&cx, &cy, PANEL_NOTIFY_X(sw) + PANEL_NOTIFY_W / 2, PANEL_ROW(sh), 0);
    mouse_click(1);
    ktest_wait_idle(800);
    int hx = PANEL_NOTIFY_X(sw) + PANEL_NOTIFY_W - HIST_W, hy = sh - PANEL_H + 4 - HIST_H(3);
    ktest_assert(pixel(hx + 2, hy + 2) == CARD_BG, "history above the bell: %08x", pixel(hx + 2, hy + 2));
    kprintf("gui_notify: history shown\n");
    sleep_ms(1500);                     /* the screendump of the QMP script */

    /* The switch in the footer turns on do-not-disturb. A new normal
     * notification then shows no pop-up. */
    mouse_move_to(&cx, &cy, hx + HIST_W - 30, hy + 6 + 32 + 3 * 46 + 16, 0);
    mouse_click(1);
    ktest_wait_idle(600);
    mouse_move_to(&cx, &cy, PANEL_NOTIFY_X(sw) + PANEL_NOTIFY_W / 2, PANEL_ROW(sh), 0);
    mouse_click(1);
    ktest_wait_idle(600);
    run_shell("notify-send 'Quiet' 'No pop-up while do not disturb is on'");
    ktest_wait_idle(600);
    ktest_assert(pixel(x0 + 4, y0 + 4) != CARD_BG, "no pop-up with do not disturb: %08x", pixel(x0 + 4, y0 + 4));
    kprintf("gui_notify: do not disturb ok\n");

    stop_server(srv);
    signal_send(notifyd, SIGTERM);
    status = proc_reap(notifyd);
    ktest_assert(status == 0, "notifyd status 0x%x", status);
    kprintf("gui_notify: ok\n");
}
KTEST_DEFINE("gui_notify", test_gui_notify);
