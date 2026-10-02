/* I3: the pinyin engine of imed.  A Shift tap selects it in gedit.  Words,
 * a sentence, an abbreviation, a full-width comma, a fixed first word,
 * Backspace, a candidate of the second page and its learning, Enter for
 * the letters, and a Shift tap that commits the letters.  The abbreviation
 * zg finds 中国 first because it was chosen before. */
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

static void shift_tap(void)
{
    ps2kbd_feed_scancode(0x2a);
    ps2kbd_feed_scancode(0xaa);
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
#define BACKSPACE 0x0e
#define COMMA 0x33
#define EQUAL 0x0d
#define KEY1 0x02
#define KEY2 0x03

static void test_ime_pinyin(void)
{
    install_app("gedit");
    ktest_assert(fb_screen_present, "no framebuffer");
    sh("rm -rf /imepy.txt /home/.config/imed");
    struct proc *srv = start_server();
    struct proc *imed = proc_create_user("/bin/imed", (char *const[]){ "imed", NULL }, env, &kernel_proc);
    ktest_assert(imed != NULL, "cannot start imed");
    sleep_ms(500);
    struct proc *cl = proc_create_user("/home/.local/bin/gedit", (char *const[]){ "gedit", "/imepy.txt", NULL },
                                       env, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start gedit");
    sleep_ms(1500);

    shift_tap();                    /* the pinyin engine */
    type("zhongguo");
    tap(SPACE);                     /* 中国 */
    type("nihao");
    tap(SPACE);                     /* 你好 */
    tap(COMMA);                     /* ， */
    type("jintiantianqihenhao");
    sleep_ms(200);
    tap(SPACE);                     /* the sentence 今天天气很好 */
    type("zg");
    tap(SPACE);                     /* 中国 by its initials, first since it was chosen */
    type("woaibeijing");
    tap(KEY2);                      /* 我爱 is fixed, beijing remains */
    tap(SPACE);                     /* 北京 */
    type("zhongguox");
    tap(BACKSPACE);
    tap(SPACE);                     /* 中国 */
    type("shi");
    tap(EQUAL);
    tap(KEY1);                      /* the sixth candidate, 式 */
    type("shi");
    tap(SPACE);                     /* 式 again: learned */
    type("hello");
    tap(ENTER);                     /* the letters */
    type("shu");
    shift_tap();                    /* the letters, and the layout */
    type("x");
    sleep_ms(300);
    ctrl_key(0x1f);
    sleep_ms(500);

    struct file *f;
    ktest_assert(vfs_open("/imepy.txt", O_RDONLY, 0, &f) == 0, "open /imepy.txt");
    char buf[256];
    long n = file_read(f, buf, sizeof buf - 1);
    file_put(f);
    buf[n > 0 ? n : 0] = '\0';
    kprintf("ime_pinyin: gedit wrote %s\n", buf);
    ktest_assert(strcmp(buf, "中国你好，今天天气很好中国我爱北京中国式式helloshux") == 0, "gedit text '%s'", buf);
    kprintf("ime_pinyin: text ok\n");
    sh("cat /home/.config/imed/pinyin.user");
    alt_key(0x3e);
    proc_reap(cl);
    signal_send(imed, SIGTERM);
    proc_reap(imed);
    stop_server(srv);
    sh("rm -rf /imepy.txt /home/.config/imed");
}
KTEST_DEFINE("ime_pinyin", test_ime_pinyin);
