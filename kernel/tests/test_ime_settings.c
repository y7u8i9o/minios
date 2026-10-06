/* I5: the settings of the input methods, the menu of the panel and the
 * terminal.  In the terminal the layout types "echo ", the panel menu
 * selects the pinyin engine, which composes 你好 and, with a page size of 3
 * from the settings, ignores the digit 4 for shi before Space chooses 是.
 * A Shift tap returns to the layout for the redirection.  The settings then
 * leave only the Japanese engine, which the menu offers and selects. */
#include <tests/ktest.h>
#include <drivers/ps2kbd.h>
#include <drivers/fbdev.h>
#include <drivers/timer.h>
#include <sched/user.h>
#include <sched/proc.h>
#include <fs/vfs.h>
#include <ipc/signal.h>
#include <lib/string.h>
#include <console.h>
#include "gui_helpers.h"

static char *const env[] = { "PATH=/bin", "HOME=/home", NULL };

/* The panel geometry of user/panel/panel.h and imemenu.c. */
#define MENU_W 220
#define ROW_H 28
#define MENU_PAD 6

static void tap(uint8_t code)
{
    ps2kbd_feed_scancode(code);
    ps2kbd_feed_scancode((uint8_t)(code | 0x80));
    ktest_wait_idle(40);
}

static void shifted(uint8_t code)
{
    ps2kbd_feed_scancode(0x2a);
    tap(code);
    ps2kbd_feed_scancode(0xaa);
    ktest_wait_idle(40);
}

static const uint8_t letter_code[26] = {
    0x1e, 0x30, 0x2e, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
    0x31, 0x18, 0x19, 0x10, 0x13, 0x1f, 0x14, 0x16, 0x2f, 0x11, 0x2d, 0x15, 0x2c,
};

static void type(const char *s)
{
    for (; *s; s++)
        tap(*s == ' ' ? 0x39 : letter_code[*s - 'a']);
}

static void sh(const char *command)
{
    struct proc *p = proc_create_user("/bin/sh", (char *const[]){ "sh", "-c", (char *)command, NULL }, env, &kernel_proc);
    ktest_assert(p != NULL, "cannot start sh");
    proc_reap(p);
}

static int label_right(void)
{
    return PANEL_INPUT_X(logical_w()) + PANEL_INPUT_W;
}

/* choose opens the menu of the panel label and clicks method row of n. */
static void choose(int row, int n)
{
    int cx = logical_w() / 2, cy = logical_h() / 2;
    mouse_move_to(&cx, &cy, label_right() - PANEL_INPUT_W / 2, PANEL_ROW(logical_h()), 0);
    mouse_click(1);
    ktest_wait_idle(800);
    int top = logical_h() - PANEL_H + 4 - (2 * MENU_PAD + n * ROW_H);
    mouse_move_to(&cx, &cy, label_right() - MENU_W / 2, top + MENU_PAD + row * ROW_H + ROW_H / 2, 0);
    mouse_click(1);
    ktest_wait_idle(500);
}

static void test_gui_ime_settings(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    sh("rm -rf /imeterm /home/.config/desktop.conf /home/.config/imed");
    struct proc *srv = start_server();
    struct proc *imed = proc_create_user("/bin/imed", (char *const[]){ "imed", NULL }, env, &kernel_proc);
    ktest_assert(imed != NULL, "cannot start imed");
    struct proc *term = proc_create_user("/bin/term", (char *const[]){ "term", NULL }, env, &kernel_proc);
    ktest_assert(term != NULL, "cannot start the terminal");
    ktest_wait_idle(2500);
    sh("settings set ime_page_size 3");

    type("echo ");
    choose(1, 3);                   /* layout, pinyin, japanese: pinyin */
    type("nihao");
    tap(0x39);                      /* 你好 */
    type("shi");
    tap(0x05);                      /* 4 is beyond the page of 3 */
    tap(0x39);                      /* 是 */
    ps2kbd_feed_scancode(0x2a);
    ps2kbd_feed_scancode(0xaa);     /* a Shift tap: the layout */
    ktest_wait_idle(200);
    tap(0x39);
    shifted(0x34);                  /* > */
    tap(0x35);                      /* / */
    type("imeterm");
    tap(0x1c);
    ktest_wait_idle(800);

    sh("settings set ime_engines japanese");
    ktest_wait_idle(1500);
    choose(1, 2);                   /* layout, japanese: japanese */

    struct file *f;
    ktest_assert(vfs_open("/imeterm", O_RDONLY, 0, &f) == 0, "open /imeterm");
    char buf[64];
    long n = file_read(f, buf, sizeof buf - 1);
    file_put(f);
    buf[n > 0 ? n : 0] = '\0';
    kprintf("gui_ime_settings: the shell wrote %s", buf);
    ktest_assert(strcmp(buf, "你好是\n") == 0, "the shell wrote '%s'", buf);
    signal_send(term, SIGTERM);
    proc_reap(term);
    signal_send(imed, SIGTERM);
    proc_reap(imed);
    stop_server(srv);
    sh("rm -rf /imeterm /home/.config/desktop.conf /home/.config/imed");
    kprintf("gui_ime_settings: ok\n");
}
KTEST_DEFINE("gui_ime_settings", test_gui_ime_settings);
