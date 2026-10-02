/* L6 and I0: input methods.  A Ctrl+Shift tap selects the Japanese engine,
 * then the Chinese engine, a Shift tap the layout, the Chinese engine again
 * and the layout, and Ctrl+Space the Chinese engine.  Keys typed into gedit
 * compose kanji from romaji and hanzi from pinyin through the candidates
 * of the compositor, and the keys that a composition uses do not reach
 * gedit. */
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
/* The panel geometry of user/panel/panel.h. */
#define CLOCK_W 80
#define MIXER_W 30
#define INPUT_W 30
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
    struct proc *panel = proc_create_user("/bin/panel", (char *const[]){ "panel", NULL }, (char *const[]){ NULL },
                                          &kernel_proc);
    ktest_assert(panel != NULL, "cannot start the panel");
    sleep_ms(800);
    struct proc *cl = proc_create_user("/home/.local/bin/gedit", (char *const[]){ "gedit", "/ime.txt", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start gedit");
    sleep_ms(1500);

    ctrl_shift_tap();               /* Japanese */
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

    ctrl_shift_tap();               /* Chinese */
    type("zhongguo");
    tap(SPACE);                     /* 中, then the candidates of guo */
    tap(SPACE);                     /* 国 */
    type("nihao");
    tap(SPACE);
    tap(SPACE);
    sleep_ms(300);

    shift_tap();                    /* the layout */
    type("a");
    shift_tap();                    /* the Chinese engine again */
    type("hao");
    tap(SPACE);
    shift_tap();                    /* the layout */
    type("b");
    ctrl_space();                   /* the Chinese engine */
    type("ni");
    tap(SPACE);
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
    ktest_assert(strcmp(buf, "山水二かなカナ中国你好a好b你") == 0, "gedit text '%s'", buf);
    alt_key(0x3e);
    int status = proc_reap(cl);
    ktest_assert(status == 0, "gedit status 0x%x", status);
    /* A click on the label of the panel selects the next method: after the
     * Chinese engine the layout. */
    int cx = logical_w() / 2, cy = logical_h() / 2;
    mouse_move_to(&cx, &cy, logical_w() - CLOCK_W - MIXER_W - 4 - 4 - INPUT_W / 2, logical_h() - 14, 0);
    mouse_click(1);
    sleep_ms(500);
    signal_send(panel, SIGTERM);
    proc_reap(panel);
    stop_server(srv);
    run("/bin/sh", (char *const[]){ "sh", "-c", "rm -f /ime.txt", NULL });
    kprintf("ime: text ok\n");
}
KTEST_DEFINE("ime", test_ime);
