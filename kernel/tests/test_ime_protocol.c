/* I1: the input method protocol.  imed runs with its test engine, which a
 * Ctrl+Shift tap selects.  Letters compose in gedit, Space, a digit and
 * the cursor keys choose candidates, a key that the engine does not use
 * reaches gedit, a key without a reply in time reaches it as well, and a
 * Shift tap commits the composition when it selects the layout. */
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
    ktest_wait_idle(30);
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
    ktest_wait_idle(200);
}

static void extended(uint8_t code)
{
    const uint8_t seq[] = { 0xe0, code, 0xe0, (uint8_t)(code | 0x80) };
    keys(seq, sizeof seq);
}

static void test_ime_protocol(void)
{
    install_app("gedit");
    ktest_assert(fb_screen_present, "no framebuffer");
    vfs_unlink("/imeproto.txt");
    struct proc *srv = start_server();
    struct proc *imed = proc_create_user("/bin/imed", (char *const[]){ "imed", "-t", NULL }, env, &kernel_proc);
    ktest_assert(imed != NULL, "cannot start imed");
    ktest_wait_idle(500);
    struct proc *cl = proc_create_user("/usr/bin/gedit", (char *const[]){ "gedit", "/imeproto.txt", NULL },
                                       env, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start gedit");
    ktest_wait_idle(1500);

    const uint8_t ctrl_shift[] = { 0x1d, 0x2a, 0xaa, 0x9d }, shift[] = { 0x2a, 0xaa };
    keys(ctrl_shift, sizeof ctrl_shift);    /* the test engine */
    type("abc");
    ktest_wait_idle(300);
    tap(0x39);                              /* ABC */
    type("de");
    tap(0x03);                              /* the second candidate: de */
    type("fg");
    extended(0x4d);
    extended(0x4d);                         /* the third candidate: Fg */
    tap(0x39);
    tap(0x1c);                              /* not used: a new line in gedit */
    tap(0x58);                              /* F12: no reply in time, gedit receives it */
    ktest_wait_idle(1700);
    type("hi");
    keys(shift, sizeof shift);              /* the layout: hi is committed */
    type("x");
    ktest_wait_idle(300);
    ctrl_key(0x1f);
    ktest_wait_idle(500);

    struct file *f;
    ktest_assert(vfs_open("/imeproto.txt", O_RDONLY, 0, &f) == 0, "open /imeproto.txt");
    char buf[64];
    long n = file_read(f, buf, sizeof buf - 1);
    file_put(f);
    buf[n > 0 ? n : 0] = '\0';
    kprintf("ime_protocol: gedit wrote %s\n", buf);
    ktest_assert(strcmp(buf, "ABCdeFg\nhix") == 0, "gedit text '%s'", buf);
    kprintf("ime_protocol: text ok\n");
    alt_key(0x3e);
    int status = proc_reap(cl);
    ktest_assert(status == 0, "gedit status 0x%x", status);
    signal_send(imed, SIGTERM);
    proc_reap(imed);
    ktest_wait_idle(300);
    stop_server(srv);
    vfs_unlink("/imeproto.txt");
}
KTEST_DEFINE("ime_protocol", test_ime_protocol);
