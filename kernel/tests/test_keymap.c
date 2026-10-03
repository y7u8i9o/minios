/* L5: keyboard layouts.  loadkeys sets the fr, es, ru and jp layouts of
 * the console, and scancodes produce their characters, the compositions
 * of dead keys, the AltGr level, the second group after Alt+Shift and the
 * keys of the Japanese keyboard.  The compositor then uses the fr and ru
 * layouts for gedit. */
#include <tests/ktest.h>
#include <drivers/ps2kbd.h>
#include <drivers/tty.h>
#include <drivers/fbdev.h>
#include <drivers/timer.h>
#include <sched/user.h>
#include <sched/proc.h>
#include <ipc/signal.h>
#include <fs/vfs.h>
#include <lib/string.h>
#include <console.h>
#include <lib/printf.h>
#include <errno.h>
#include "gui_helpers.h"

static void run(const char *path, char *const argv[])
{
    struct proc *p = proc_create_user(path, argv, (char *const[]){ "PATH=/bin", "HOME=/home", NULL }, &kernel_proc);
    ktest_assert(p != NULL, "cannot start %s", path);
    int status = proc_reap(p);
    ktest_assert(status == 0, "%s status 0x%x", path, status);
}

static void loadkeys(const char *name)
{
    run("/bin/loadkeys", (char *const[]){ "loadkeys", (char *)name, NULL });
}

/* Key presses: a scancode, or 0xe0 followed by one.  A press of a
 * modifier stays down until the same code with 0x80 releases it. */
static void keys(const uint8_t *codes, size_t n)
{
    for (size_t i = 0; i < n; i++)
        ps2kbd_feed_scancode(codes[i]);
}

static void tap(uint8_t code)
{
    ps2kbd_feed_scancode(code);
    ps2kbd_feed_scancode((uint8_t)(code | 0x80));
}

static void shifted(uint8_t code)
{
    ps2kbd_feed_scancode(0x2a);
    tap(code);
    ps2kbd_feed_scancode(0xaa);
}

static void altgr(uint8_t code)
{
    const uint8_t down[] = { 0xe0, 0x38 }, up[] = { 0xe0, 0xb8 };
    keys(down, 2);
    tap(code);
    keys(up, 2);
}

static void alt_shift(void)
{
    const uint8_t seq[] = { 0x38, 0x2a, 0xaa, 0xb8 };
    keys(seq, sizeof seq);
}

/* expect_line reads the console line that the keys produced. */
static void expect_line(const char *want, const char *layout)
{
    tap(0x1c);
    char got[64];
    size_t n = 0;
    int c;
    while (n + 1 < sizeof got && (c = tty_getc(&console_tty)) >= 0) {
        got[n++] = (char)c;
        if (c == '\n')
            break;
    }
    got[n] = '\0';
    kprintf("keymap: %s console line %s", layout, got);
    ktest_assert(strcmp(got, want) == 0, "%s layout produced '%s', expected '%s'", layout, got, want);
}

static void test_console(void)
{
    while (tty_getc(&console_tty) >= 0)
        ;
    loadkeys("fr");
    tap(0x10);                      /* the key of q: a */
    tap(0x1e);                      /* the key of a: q */
    tap(0x1a);                      /* dead circumflex */
    tap(0x12);                      /* e */
    altgr(0x0b);                    /* AltGr+0: @ */
    tap(0x03);                      /* é */
    shifted(0x02);                  /* Shift+&: 1 */
    expect_line("aq\xc3\xaa@\xc3\xa9" "1\n", "fr");

    loadkeys("es");
    tap(0x27);                      /* ñ */
    tap(0x28);                      /* dead acute */
    tap(0x1e);                      /* a */
    shifted(0x28);                  /* dead diaeresis */
    tap(0x16);                      /* u */
    altgr(0x03);                    /* AltGr+2: @ */
    tap(0x0d);                      /* ¡ */
    tap(0x28);                      /* dead acute, then a space: the accent */
    tap(0x39);
    expect_line("\xc3\xb1\xc3\xa1\xc3\xbc@\xc2\xa1\xc2\xb4\n", "es");

    loadkeys("ru");
    tap(0x10);                      /* q in the first group */
    alt_shift();
    tap(0x10);                      /* й */
    tap(0x21);                      /* а */
    shifted(0x29);                  /* Ё */
    alt_shift();
    tap(0x10);                      /* q again */
    expect_line("q\xd0\xb9\xd0\xb0\xd0\x81q\n", "ru");

    loadkeys("jp");
    tap(0x7d);                      /* the yen key: backslash */
    shifted(0x7d);                  /* | */
    tap(0x0d);                      /* ^ */
    shifted(0x03);                  /* " */
    shifted(0x73);                  /* the ro key: _ */
    expect_line("\\|^\"_\n", "jp");
    loadkeys("us");
}

/* set_keymap writes the keymap setting and asks the compositor to read it.
 * The compositor runs as root without HOME and reads the configuration in
 * root's home (docs/design/users.md). */
static void set_keymap(const char *name)
{
    char command[96];
    ksnprintf(command, sizeof command, "mkdir -p /root/.config && echo keymap=%s > /root/.config/desktop.conf", name);
    run("/bin/sh", (char *const[]){ "sh", "-c", command, NULL });
    run("/bin/x12settings", (char *const[]){ "x12settings", "set", "keymap_reload", "1", NULL });
    sleep_ms(300);
}

static void test_compositor(void)
{
    install_app("gedit");
    ktest_assert(fb_screen_present, "no framebuffer");
    vfs_unlink("/keymap.txt");
    struct proc *srv = start_server();
    set_keymap("fr");
    struct proc *cl = proc_create_user("/usr/bin/gedit", (char *const[]){ "gedit", "/keymap.txt", NULL },
                                       (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start gedit");
    sleep_ms(1500);
    tap(0x10);                      /* a */
    tap(0x1a);                      /* dead circumflex */
    tap(0x12);                      /* ê */
    altgr(0x0b);                    /* @ */
    sleep_ms(300);
    set_keymap("ru");
    alt_shift();
    tap(0x10);                      /* й */
    tap(0x21);                      /* а */
    sleep_ms(300);
    ctrl_key(0x1f);                 /* Ctrl+S */
    sleep_ms(500);
    struct file *f;
    ktest_assert(vfs_open("/keymap.txt", O_RDONLY, 0, &f) == 0, "open /keymap.txt");
    char buf[64];
    long n = file_read(f, buf, sizeof buf - 1);
    file_put(f);
    buf[n > 0 ? n : 0] = '\0';
    kprintf("keymap: gedit wrote %s\n", buf);
    ktest_assert(strcmp(buf, "a\xc3\xaa@\xd0\xb9\xd0\xb0") == 0, "gedit text '%s'", buf);
    alt_key(0x3e);
    int status = proc_reap(cl);
    ktest_assert(status == 0, "gedit status 0x%x", status);
    stop_server(srv);
    run("/bin/sh", (char *const[]){ "sh", "-c", "rm -f /root/.config/desktop.conf /keymap.txt", NULL });
}

static void test_keymap(void)
{
    test_console();
    test_compositor();
    kprintf("keymap: layouts ok\n");
}
KTEST_DEFINE("keymap", test_keymap);
