/* I2: the candidate window of imed.  With a page size of 2 the test engine
 * shows its three candidates on two pages below the caret.  A click on the
 * second candidate of a horizontal page chooses it, the wheel turns to the
 * second page, and a click on the second candidate of a vertical page
 * chooses it. */
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

#define SELECTION 0x003c78c8u       /* TC_SELECTION of the default theme */

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

static void sh(const char *command)
{
    struct proc *p = proc_create_user("/bin/sh", (char *const[]){ "sh", "-c", (char *)command, NULL }, env, &kernel_proc);
    ktest_assert(p != NULL, "cannot start sh");
    proc_reap(p);
}

/* highlight finds the selected candidate: the bounding box of the pixels
 * of the selection colour inside the text area of gedit, below the
 * underline of the preedit on the first line and inside the blue border. */
static int highlight(int *x0, int *y0, int *x1, int *y1)
{
    *x0 = *y0 = 100000;
    *x1 = *y1 = -1;
    for (int y = 158; y < 500; y++)
        for (int x = 48; x < 698; x++)
            if (pixel(x, y) == SELECTION) {
                if (x < *x0) *x0 = x;
                if (y < *y0) *y0 = y;
                if (x > *x1) *x1 = x;
                if (y > *y1) *y1 = y;
            }
    return *x1 >= 0;
}

static void click(int x, int y)
{
    int cx = logical_w() / 2, cy = logical_h() / 2;
    mouse_move_to(&cx, &cy, x, y, 0);
    ktest_wait_idle(100);
    mouse_click(1);
    ktest_wait_idle(400);
}

static void test_ime_candidates(void)
{
    install_app("gedit");
    ktest_assert(fb_screen_present, "no framebuffer");
    vfs_unlink("/imecand.txt");
    sh("mkdir -p /home/.config && printf 'ime_page_size=2\\n' > /home/.config/desktop.conf");
    struct proc *srv = start_server();
    struct proc *imed = proc_create_user("/bin/imed", (char *const[]){ "imed", "-t", NULL }, env, &kernel_proc);
    ktest_assert(imed != NULL, "cannot start imed");
    ktest_wait_idle(500);
    struct proc *cl = proc_create_user("/usr/bin/gedit", (char *const[]){ "gedit", "/imecand.txt", NULL },
                                       env, &kernel_proc);
    ktest_assert(cl != NULL, "cannot start gedit");
    ktest_wait_idle(1500);
    const uint8_t ctrl_shift[] = { 0x1d, 0x2a, 0xaa, 0x9d };
    for (size_t i = 0; i < sizeof ctrl_shift; i++)
        ps2kbd_feed_scancode(ctrl_shift[i]);
    ktest_wait_idle(200);

    type("abc");
    sleep_ms(500);
    int x0, y0, x1, y1;
    ktest_assert(highlight(&x0, &y0, &x1, &y1), "no candidate window below the caret");
    kprintf("ime_candidates: horizontal page at %d,%d\n", x0, y0);
    click(x1 + 30, (y0 + y1) / 2);          /* the second candidate: abc */

    type("de");
    sleep_ms(400);
    ktest_assert(highlight(&x0, &y0, &x1, &y1), "no candidate window for de");
    int cx = logical_w() / 2, cy = logical_h() / 2;
    mouse_move_to(&cx, &cy, x0 + 5, (y0 + y1) / 2, 0);
    ktest_wait_idle(100);
    feed_packet_wheel(0, 0, 0, 1);          /* down: the second page, De */
    sleep_ms(400);
    tap(0x39);

    sh("printf 'ime_page_size=2\\nime_orientation=vertical\\n' > /home/.config/desktop.conf");
    type("fg");
    sleep_ms(500);
    ktest_assert(highlight(&x0, &y0, &x1, &y1), "no candidate window for fg");
    kprintf("ime_candidates: vertical page at %d,%d\n", x0, y0);
    click(x0 + 10, y1 + 12);                /* the candidate below: fg */
    ktest_wait_idle(300);
    ctrl_key(0x1f);
    ktest_wait_idle(500);

    struct file *f;
    ktest_assert(vfs_open("/imecand.txt", O_RDONLY, 0, &f) == 0, "open /imecand.txt");
    char buf[64];
    long n = file_read(f, buf, sizeof buf - 1);
    file_put(f);
    buf[n > 0 ? n : 0] = '\0';
    kprintf("ime_candidates: gedit wrote %s\n", buf);
    ktest_assert(strcmp(buf, "abcDefg") == 0, "gedit text '%s'", buf);
    kprintf("ime_candidates: text ok\n");
    alt_key(0x3e);
    proc_reap(cl);
    signal_send(imed, SIGTERM);
    proc_reap(imed);
    stop_server(srv);
    sh("rm -f /home/.config/desktop.conf /imecand.txt");
}
KTEST_DEFINE("ime_candidates", test_ime_candidates);
