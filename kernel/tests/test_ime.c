/* L6: input methods.  Super+Space selects the Japanese engine, then the
 * Chinese engine, then the layout again.  Keys typed into gedit compose
 * kanji from romaji and hanzi from pinyin through the candidates of the
 * compositor, and the keys that a composition uses do not reach gedit. */
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

static void super_space(void)
{
    const uint8_t down[] = { 0xe0, 0x5b }, up[] = { 0xe0, 0xdb };
    ps2kbd_feed_scancode(down[0]);
    ps2kbd_feed_scancode(down[1]);
    tap(0x39);
    ps2kbd_feed_scancode(up[0]);
    ps2kbd_feed_scancode(up[1]);
    sleep_ms(200);
}

#define SPACE 0x39
#define ENTER 0x1c
#define F7    0x41
#define KEY2  0x03

static void run(const char *path, char *const argv[])
{
    struct proc *p = proc_create_user(path, argv, (char *const[]){ "PATH=/bin", "HOME=/home", NULL }, &kernel_proc);
    ktest_assert(p != NULL, "cannot start %s", path);
    int status = proc_reap(p);
    ktest_assert(status == 0, "%s status 0x%x", path, status);
}

static void test_ime(void)
{
    install_app("gedit");
    ktest_assert(fb_screen_present, "no framebuffer");
    vfs_unlink("/ime.txt");
    struct proc *srv = start_server();
    struct proc *cl = proc_create_user("/home/.local/bin/gedit", (char *const[]){ "gedit", "/ime.txt", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start gedit");
    sleep_ms(1500);

    super_space();                  /* Japanese */
    type("yama");
    tap(SPACE);                     /* 山 is the first candidate of やま */
    tap(ENTER);
    type("kawa");
    tap(SPACE);
    tap(SPACE);                     /* the second candidate of かわ: 水 */
    tap(ENTER);
    type("ni");
    tap(SPACE);
    tap(KEY2);                      /* the second candidate of に: 二 */
    type("kana");
    tap(ENTER);                     /* the kana as they are */
    type("kana");
    tap(F7);
    tap(ENTER);                     /* in katakana */
    sleep_ms(300);

    super_space();                  /* Chinese */
    type("zhongguo");
    tap(SPACE);                     /* 中, then the candidates of guo */
    tap(SPACE);                     /* 国 */
    type("nihao");
    tap(SPACE);
    tap(SPACE);
    sleep_ms(300);

    super_space();                  /* the layout */
    type("a");
    sleep_ms(300);
    ctrl_key(0x1f);                 /* Ctrl+S */
    sleep_ms(500);

    struct file *f;
    ktest_assert(vfs_open("/ime.txt", O_RDONLY, 0, &f) == 0, "open /ime.txt");
    char buf[128];
    long n = file_read(f, buf, sizeof buf - 1);
    file_put(f);
    buf[n > 0 ? n : 0] = '\0';
    kprintf("ime: gedit wrote %s\n", buf);
    ktest_assert(strcmp(buf, "山水二かなカナ中国你好a") == 0, "gedit text '%s'", buf);
    alt_key(0x3e);
    int status = proc_reap(cl);
    ktest_assert(status == 0, "gedit status 0x%x", status);
    stop_server(srv);
    run("/bin/sh", (char *const[]){ "sh", "-c", "rm -f /ime.txt", NULL });
    kprintf("ime: text ok\n");
}
KTEST_DEFINE("ime", test_ime);
