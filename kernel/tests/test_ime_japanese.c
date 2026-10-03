/* I4: the Japanese engine of imed.  Two Ctrl+Shift taps select it in
 * gedit.  A word, a sentence of two segments, a lengthened first segment,
 * a candidate chosen by its number and then learned, katakana by F7, and
 * Escape back to the kana. */
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

static void tap(uint8_t code)
{
    ps2kbd_feed_scancode(code);
    ps2kbd_feed_scancode((uint8_t)(code | 0x80));
    sleep_ms(30);
}

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

static void sh(const char *command)
{
    struct proc *p = proc_create_user("/bin/sh", (char *const[]){ "sh", "-c", (char *)command, NULL }, env, &kernel_proc);
    ktest_assert(p != NULL, "cannot start sh");
    proc_reap(p);
}

#define SPACE 0x39
#define ENTER 0x1c
#define ESC   0x01
#define F7    0x41
#define KEY4  0x05

static void test_ime_japanese(void)
{
    install_app("gedit");
    ktest_assert(fb_screen_present, "no framebuffer");
    sh("rm -rf /imejp.txt /home/.config/imed");
    struct proc *srv = start_server();
    struct proc *imed = proc_create_user("/bin/imed", (char *const[]){ "imed", NULL }, env, &kernel_proc);
    ktest_assert(imed != NULL, "cannot start imed");
    sleep_ms(800);
    struct proc *cl = proc_create_user("/usr/bin/gedit", (char *const[]){ "gedit", "/imejp.txt", NULL },
                                       env, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start gedit");
    sleep_ms(1500);

    const uint8_t ctrl_shift[] = { 0x1d, 0x2a, 0xaa, 0x9d };
    keys(ctrl_shift, sizeof ctrl_shift);    /* pinyin */
    keys(ctrl_shift, sizeof ctrl_shift);    /* Japanese */
    type("nihongo");
    tap(SPACE);
    tap(ENTER);                             /* 日本語 */
    type("watashihagakuseidesu");
    tap(SPACE);
    tap(ENTER);                             /* 私は学生です */
    type("kyouhaiitenkidesune");
    tap(SPACE);                             /* 今日は｜いい天気ですね */
    const uint8_t shift_right[] = { 0x2a, 0xe0, 0x4d, 0xe0, 0xcd, 0xaa };
    keys(shift_right, sizeof shift_right);  /* 今日はい｜移転機ですね */
    tap(ENTER);
    type("kanji");
    tap(SPACE);                             /* 感じ */
    tap(SPACE);                             /* the candidates */
    tap(KEY4);                              /* 漢字 */
    tap(ENTER);
    type("kanji");
    tap(SPACE);                             /* 漢字, learned */
    tap(ENTER);
    type("katakana");
    tap(F7);
    tap(ENTER);                             /* カタカナ */
    type("yama");
    tap(SPACE);
    tap(ESC);
    tap(ENTER);                             /* やま */
    sleep_ms(300);
    ctrl_key(0x1f);
    sleep_ms(500);

    struct file *f;
    ktest_assert(vfs_open("/imejp.txt", O_RDONLY, 0, &f) == 0, "open /imejp.txt");
    char buf[256];
    long n = file_read(f, buf, sizeof buf - 1);
    file_put(f);
    buf[n > 0 ? n : 0] = '\0';
    kprintf("ime_japanese: gedit wrote %s\n", buf);
    ktest_assert(strcmp(buf, "日本語私は学生です今日はい移転機ですね漢字漢字カタカナやま") == 0, "gedit text '%s'", buf);
    kprintf("ime_japanese: text ok\n");
    sh("cat /home/.config/imed/japanese.user");
    alt_key(0x3e);
    proc_reap(cl);
    signal_send(imed, SIGTERM);
    proc_reap(imed);
    stop_server(srv);
    sh("rm -rf /imejp.txt /home/.config/imed");
}
KTEST_DEFINE("ime_japanese", test_ime_japanese);
