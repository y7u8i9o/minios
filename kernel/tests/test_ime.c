/* I0 and I4: the switch keys of the input methods, with imed.  A
 * Ctrl+Shift tap selects the pinyin engine, another one the Japanese
 * engine, a Shift tap the layout and the Japanese engine again, Ctrl+Space
 * the layout, Zenkaku/Hankaku the Japanese engine, the Eisu key (LANG2)
 * the layout, and a click on the panel label the pinyin engine. */
#include <tests/ktest.h>
#include <drivers/ps2kbd.h>
#include <drivers/fbdev.h>
#include <drivers/timer.h>
#include <sched/user.h>
#include <sched/proc.h>
#include <fs/vfs.h>
#include <lib/string.h>
#include <lib/printf.h>
#include <console.h>
#include <ipc/signal.h>
#include <errno.h>
#include "gui_helpers.h"

static void tap(uint8_t code)
{
    ps2kbd_feed_scancode(code);
    ps2kbd_feed_scancode((uint8_t)(code | 0x80));
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

static void keys(const uint8_t *codes, size_t n)
{
    for (size_t i = 0; i < n; i++)
        ps2kbd_feed_scancode(codes[i]);
    sleep_ms(200);
}

static void ctrl_shift_tap(void)
{
    const uint8_t seq[] = { 0x1d, 0x2a, 0xaa, 0x9d };
    keys(seq, sizeof seq);
}

static void shift_tap(void)
{
    const uint8_t seq[] = { 0x2a, 0xaa };
    keys(seq, sizeof seq);
}

static void ctrl_space(void)
{
    const uint8_t seq[] = { 0x1d, 0x39, 0xb9, 0x9d };
    keys(seq, sizeof seq);
}

#define SPACE 0x39
#define ZENKAKU 0x55             /* the scancode of Zenkaku/Hankaku, key 85 */
/* The panel geometry of user/panel/panel.h. */
#define CLOCK_W 80
#define MIXER_W 30
#define INPUT_W 30
#define ENTER 0x1c

static void test_ime(void)
{
    install_app("gedit");
    ktest_assert(fb_screen_present, "no framebuffer");
    vfs_unlink("/ime.txt");
    struct proc *srv = start_server();
    char *const env[] = { "PATH=/bin", "HOME=/home", NULL };
    struct proc *imed = proc_create_user("/bin/imed", (char *const[]){ "imed", NULL }, env, &kernel_proc);
    ktest_assert(imed != NULL, "cannot start imed");
    struct proc *panel = proc_create_user("/bin/panel", (char *const[]){ "panel", NULL }, env, &kernel_proc);
    ktest_assert(panel != NULL, "cannot start the panel");
    sleep_ms(800);
    struct proc *cl = proc_create_user("/home/.local/bin/gedit", (char *const[]){ "gedit", "/ime.txt", NULL },
                                       env, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start gedit");
    sleep_ms(1500);

    ctrl_shift_tap();               /* pinyin */
    type("nihao");
    tap(SPACE);                     /* 你好 */
    ctrl_shift_tap();               /* Japanese */
    type("nihongo");
    tap(SPACE);
    tap(ENTER);                     /* 日本語 */
    shift_tap();                    /* the layout */
    type("a");
    shift_tap();                    /* the Japanese engine again */
    type("yama");
    tap(SPACE);
    tap(ENTER);                     /* 山 */
    ctrl_space();                   /* the layout */
    type("b");
    tap(ZENKAKU);                   /* the Japanese engine */
    type("hashi");
    tap(ENTER);                     /* はし as kana */
    ps2kbd_feed_scancode(0xf1);     /* Eisu: the layout */
    sleep_ms(200);
    type("c");
    sleep_ms(300);
    ctrl_key(0x1f);
    sleep_ms(500);

    struct file *f;
    ktest_assert(vfs_open("/ime.txt", O_RDONLY, 0, &f) == 0, "open /ime.txt");
    char buf[128];
    long n = file_read(f, buf, sizeof buf - 1);
    file_put(f);
    buf[n > 0 ? n : 0] = '\0';
    kprintf("ime: gedit wrote %s\n", buf);
    ktest_assert(strcmp(buf, "你好日本語a山bはしc") == 0, "gedit text '%s'", buf);
    alt_key(0x3e);
    int status = proc_reap(cl);
    ktest_assert(status == 0, "gedit status 0x%x", status);
    /* A click on the label of the panel selects the next method: after the
     * layout the pinyin engine. */
    int cx = logical_w() / 2, cy = logical_h() / 2;
    mouse_move_to(&cx, &cy, logical_w() - CLOCK_W - MIXER_W - 4 - 4 - INPUT_W / 2, logical_h() - 14, 0);
    mouse_click(1);
    sleep_ms(500);
    signal_send(panel, SIGTERM);
    proc_reap(panel);
    signal_send(imed, SIGTERM);
    proc_reap(imed);
    stop_server(srv);
    struct proc *rm = proc_create_user("/bin/sh", (char *const[]){ "sh", "-c", "rm -rf /ime.txt /home/.config/imed", NULL },
                                       env, &kernel_proc);
    if (rm)
        proc_reap(rm);
    kprintf("ime: text ok\n");
}
KTEST_DEFINE("ime", test_ime);
