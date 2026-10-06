/* S6 of docs/plan/release-0.7.0.md: the screen locker. The tests start
 * X12 and the panel without the greeter. A root client selects the session
 * user with the setting session_uid, as the greeter does. The lock screen
 * of the locker lock has the desktop colour with a gradient. The gradient is
 * darker than the panel at the bottom of the screen. A session without a
 * locker shows black. */
#include <tests/ktest.h>
#include <drivers/fbdev.h>
#include <drivers/timer.h>
#include <sched/user.h>
#include <sched/proc.h>
#include <ipc/signal.h>
#include <console.h>
#include <fs/vfs.h>
#include "gui_helpers.h"

#define MENU_BG 0x00fafbfc
/* The number of key presses that guitest logged. */
#define KEYS_LOGGED(n) "test \"$(grep -c ' key ' /tmp/keys.log)\" = " #n

/* Super+L: the left Super key is the extended code 0x5b. */
static void super_l(void)
{
    ps2kbd_feed_scancode(0xe0);
    ps2kbd_feed_scancode(0x5b);
    press_key(0x26);
    ps2kbd_feed_scancode(0xe0);
    ps2kbd_feed_scancode(0xdb);
    ktest_wait_idle(150);
}

/* Wait until the pixel at x, y equals v (equal 1) or differs from v
 * (equal 0). The wait ends after ms milliseconds at the latest. */
static bool wait_pixel(int x, int y, uint32_t v, int equal, int ms)
{
    for (int t = 0; t < ms; t += 50) {
        if ((pixel(x, y) == v) == equal)
            return true;
        sleep_ms(50);
    }
    return false;
}

/* The panel at the bottom of the screen is hidden while the session is
 * locked. The middle of the panel shows only its background. */
static bool wait_locked(int sh)
{
    return wait_pixel(logical_w() / 2, PANEL_ROW(sh), PANEL_BG, 0, 8000);
}

static bool wait_unlocked(int sh)
{
    return wait_pixel(logical_w() / 2, PANEL_ROW(sh), PANEL_BG, 1, 8000);
}

static void test_gui_lock(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    int sw = logical_w(), sh = logical_h();
    struct proc *srv = start_server();
    run_shell("x12settings set session_uid 0");
    run_shell("printf 'secret\\nsecret\\n' | passwd");
    /* A window that logs its keys. Its second window has the focus. */
    struct proc *client = proc_create_user("/bin/sh", (char *const[]){ "sh", "-c", "guitest > /tmp/keys.log", NULL },
                                           (char *const[]){ "PATH=/bin", NULL }, &kernel_proc);
    ktest_assert(client != NULL, "cannot start guitest");
    ktest_wait_idle(1000);
    ktest_assert(pixel(sw / 2, PANEL_ROW(sh)) == PANEL_BG, "panel shown: %08x", pixel(sw / 2, PANEL_ROW(sh)));
    type_line("a");
    ktest_wait_idle(300);
    ktest_assert(shell_status(KEYS_LOGGED(1)) == 0, "the window did not receive a key before the lock");

    /* Super+L starts the locker as the session user. */
    super_l();
    ktest_assert(wait_procs("lock", 0, 1, 8000), "no locker after Super+L");
    ktest_assert(wait_locked(sh), "the panel remains visible: %08x", pixel(sw / 2, PANEL_ROW(sh)));
    ktest_assert(wait_pixel(5, sh / 2, 0, 0, 3000), "no lock screen: %08x", pixel(5, sh / 2));
    kprintf("gui_lock: locked\n");
    sleep_ms(1500);                     /* the screendump of the QMP script */

    /* The panel receives no click while the session is locked. After the
     * unlock below, the power menu must therefore be closed. */
    int cx = sw / 2, cy = sh / 2;
    mouse_move_to(&cx, &cy, PANEL_POWER_X(sw) + PANEL_POWER_W / 2, PANEL_ROW(sh), 0);
    mouse_click(1);
    ktest_wait_idle(500);
    /* Super+L starts no second locker, and a locker started by root is
     * refused while the first locker runs. */
    super_l();
    sleep_ms(1000);
    ktest_assert(count_procs("lock", 0) == 1, "%d lockers after a second Super+L", count_procs("lock", 0));
    ktest_assert(shell_status("lock") != 0, "a second locker was accepted");
    kprintf("gui_lock: second locker refused\n");

    /* A wrong password leaves the session locked. */
    type_line("wrong\n");
    sleep_ms(3500);
    ktest_assert(count_procs("lock", 0) == 1, "the locker exited after a wrong password");
    ktest_assert(pixel(sw / 2, PANEL_ROW(sh)) != PANEL_BG, "a wrong password unlocked the session");
    kprintf("gui_lock: wrong password refused\n");

    type_line("secret\n");
    ktest_assert(wait_procs("lock", 0, 0, 8000), "the locker did not exit");
    ktest_assert(wait_unlocked(sh), "the panel did not return: %08x", pixel(sw / 2, PANEL_ROW(sh)));
    int mx = PANEL_POWER_X(sw) + PANEL_POWER_W - POWER_MENU_W, my = sh - PANEL_H + 4 - POWER_MENU_H;
    ktest_assert(pixel(mx + 2, my + 2) != MENU_BG, "the panel received a click while locked");
    /* The window received none of the keys typed while the session was
     * locked, and it receives keys again. */
    ktest_assert(shell_status(KEYS_LOGGED(1)) == 0, "the window received keys while the session was locked");
    type_line("b");
    ktest_wait_idle(300);
    ktest_assert(shell_status(KEYS_LOGGED(2)) == 0, "the window received no key after the unlock");
    kprintf("gui_lock: unlocked\n");

    /* Lock in the power menu of the panel. */
    panel_power_choose(sw, sh, POWER_ROW_LOCK);
    ktest_assert(wait_procs("lock", 0, 1, 8000), "no locker after the power menu");
    ktest_assert(wait_locked(sh), "the power menu did not lock the session");
    kprintf("gui_lock: locked from the power menu\n");
    sleep_ms(1000);
    type_line("secret\n");
    ktest_assert(wait_procs("lock", 0, 0, 8000), "the locker did not exit");
    ktest_assert(wait_unlocked(sh), "the panel did not return");
    run_shell("kill $(ps | awk '$NF == \"guitest\" { print $1 }')");
    proc_reap(client);
    stop_server(srv);
    kprintf("gui_lock: ok\n");
}
KTEST_DEFINE("gui_lock", test_gui_lock);

/* A locker that ends without an unlock leaves the session locked. */
static void test_gui_lock_crash(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    int sw = logical_w(), sh = logical_h();
    struct proc *srv = start_server();
    run_shell("x12settings set session_uid 1000");
    run_shell("printf 'userpw\\nuserpw\\n' | passwd user");
    ktest_wait_idle(600);
    super_l();
    ktest_assert(wait_procs("lock", 1000, 1, 8000), "no locker of uid 1000");
    ktest_assert(wait_locked(sh), "the session is not locked");
    kprintf("gui_lock_crash: locked\n");

    run_shell("kill -9 $(ps | awk '$NF == \"lock\" { print $1 }')");
    ktest_assert(wait_pixel(5, sh / 2, 0, 1, 5000), "no black screen: %08x", pixel(5, sh / 2));
    ktest_assert(pixel(sw / 2, PANEL_ROW(sh)) == 0, "the panel is visible: %08x", pixel(sw / 2, PANEL_ROW(sh)));
    kprintf("gui_lock_crash: black screen\n");
    sleep_ms(1500);                     /* the screendump of the QMP script */

    /* Only a locker that X12 started may resume the lock. */
    ktest_assert(shell_status("doas -u user lock") != 0, "a locker of the session user resumed the lock");
    ktest_assert(pixel(5, sh / 2) == 0, "the screen is not black: %08x", pixel(5, sh / 2));
    kprintf("gui_lock_crash: foreign locker refused\n");

    /* A key press starts a new locker. */
    press_key(0x39);
    ktest_assert(wait_procs("lock", 1000, 1, 8000), "no new locker after a key press");
    ktest_assert(wait_pixel(5, sh / 2, 0, 0, 5000), "no lock screen");
    kprintf("gui_lock_crash: locker restarted\n");
    sleep_ms(1000);
    type_line("userpw\n");
    ktest_assert(wait_procs("lock", 1000, 0, 8000), "the locker did not exit");
    ktest_assert(wait_unlocked(sh), "the panel did not return");
    stop_server(srv);
    kprintf("gui_lock_crash: ok\n");
}
KTEST_DEFINE("gui_lock_crash", test_gui_lock_crash);

/* X12 locks the session after lock_timeout seconds without input. root
 * has no password on the test image. Enter therefore unlocks the session. */
static void test_lock_idle(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    int sw = logical_w(), sh = logical_h();
    struct proc *srv = start_server();
    run_shell("x12settings set session_uid 0");
    run_shell("x12settings set lock_timeout 3");
    ktest_assert(wait_procs("lock", 0, 1, 10000), "no locker after 3 s without input");
    ktest_assert(wait_locked(sh), "the session is not locked");
    kprintf("lock_idle: locked after the timeout\n");
    /* The longer timeout is set before the unlock. The idle time then
     * starts with the key that unlocks. */
    run_shell("x12settings set lock_timeout 4");
    sleep_ms(1000);
    type_line("\n");
    ktest_assert(wait_procs("lock", 0, 0, 8000), "the locker did not exit");
    ktest_assert(wait_unlocked(sh), "the panel did not return");

    /* Pointer motion every second defers the lock. */
    int cx = sw / 2, cy = sh / 2;
    for (int i = 0; i < 7; i++) {
        mouse_move_to(&cx, &cy, sw / 2 + (i % 2) * 40, sh / 2, 0);
        sleep_ms(1000);
    }
    ktest_assert(count_procs("lock", 0) == 0, "the session locked during input");
    kprintf("lock_idle: input defers the lock\n");
    ktest_assert(wait_procs("lock", 0, 1, 10000), "no locker after the input ended");
    ktest_assert(wait_locked(sh), "the session is not locked");
    run_shell("x12settings set lock_timeout 0");
    sleep_ms(1000);
    type_line("\n");
    ktest_assert(wait_procs("lock", 0, 0, 8000), "the locker did not exit");
    ktest_assert(wait_unlocked(sh), "the panel did not return");

    /* The value 0 turns the automatic lock off. */
    sleep_ms(6000);
    ktest_assert(count_procs("lock", 0) == 0, "the session locked with lock_timeout 0");
    stop_server(srv);
    kprintf("lock_idle: ok\n");
}
KTEST_DEFINE("lock_idle", test_lock_idle);
