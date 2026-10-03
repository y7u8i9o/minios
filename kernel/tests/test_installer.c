/* P10 of docs/plan/packaging.md: the graphical installer. The case boots
 * the installation medium without an answer file. init starts
 * installer-gui as the console program, which starts X12 and shows its
 * window. The test fills the window through the keyboard: Tab passes the
 * disk, the package group (desktop-system), the keyboard layout and the
 * time zone with their defaults, then the password of root, the account
 * berta in place of the proposed name user, its full name and password,
 * and Enter presses Install. When the log of the back end reports the end
 * of the installation, Tab and Enter press Power off. The second boot of
 * the case starts the installed disk. */
#include <tests/ktest.h>
#include <fs/vfs.h>
#include <sched/user.h>
#include <sched/proc.h>
#include <drivers/timer.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <lib/printf.h>
#include <console.h>
#include <errno.h>
#include "gui_helpers.h"

#define INSTALLER_LOG "/run/installer/installer.log"

static bool contains(const char *buf, size_t len, const char *want, size_t n)
{
    for (size_t i = 0; i + n <= len; i++)
        if (memcmp(buf + i, want, n) == 0)
            return true;
    return false;
}

/* Whether the log of the back end contains the line text. The log is read
 * in chunks that overlap by the length of the line. */
static bool log_has_line(const char *text)
{
    struct file *f;
    if (vfs_open(INSTALLER_LOG, O_RDONLY, 0, &f) < 0)
        return false;
    char want[96];
    size_t n = (size_t)ksnprintf(want, sizeof want, "\n%s\n", text);
    char *buf = kmalloc(4096);
    ktest_assert(buf != NULL, "alloc");
    buf[0] = '\n';                  /* the first line follows a line break too */
    size_t have = 1;
    bool found = false;
    for (;;) {
        long r = file_read(f, buf + have, 4096 - have);
        if (r <= 0)
            break;
        have += (size_t)r;
        if (contains(buf, have, want, n)) {
            found = true;
            break;
        }
        if (have > n) {
            memmove(buf, buf + have - n, n);
            have = n;
        }
    }
    kfree(buf);
    file_put(f);
    return found;
}

static void tab(int times)
{
    for (int i = 0; i < times; i++) {
        press_key(0x0f);
        sleep_ms(150);
    }
}

static void test_gui_installer(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    struct proc *init = proc_create_user("/bin/init", (char *const[]){ "/bin/init", NULL },
                                         (char *const[]){ "PATH=/bin", NULL }, &kernel_proc);
    ktest_assert(init != NULL, "cannot start /bin/init");
    proc_set_init(init);
    ktest_assert(wait_procs("x12", 0, 1, 20000), "no display server");
    /* The window is the first toplevel, whose active title bar is above
     * its contents at (40, 60). */
    for (int waited = 0; pixel(42, 50) != 0x00ebebeb && waited < 20000; waited += 100)
        sleep_ms(100);
    ktest_assert(pixel(42, 50) == 0x00ebebeb, "no installer window: %08x", pixel(42, 50));
    ktest_assert(count_procs("installer", -1) == 0, "the text installer runs");
    sleep_ms(1000);
    kprintf("gui_installer: window shown\n");

    tab(5);                         /* the password of root */
    type_line("rootsecret");
    tab(1);                         /* the account name, proposed as user */
    type_ctrl('a');
    type_line("berta");
    tab(1);
    type_line("Berta Example");
    tab(1);
    type_line("bertasecret");
    tab(1);                         /* Install */
    press_key(0x1c);
    kprintf("gui_installer: install pressed\n");

    /* The back end writes "done" or "the installation failed" as its last
     * line. The installation of the desktop takes minutes under TCG. */
    int ms = 0;
    for (; ms < 600000; ms += 1000) {
        if (log_has_line("done"))
            break;
        ktest_assert(!log_has_line("the installation failed"), "the installation failed");
        sleep_ms(1000);
    }
    ktest_assert(ms < 600000, "the installation did not end");
    kprintf("gui_installer: installed\n");
    /* The window enables Power off when its timer sees the end of the
     * child, within 300 ms. Install is disabled, and Tab moves to Power off. */
    sleep_ms(1500);
    tab(1);
    press_key(0x1c);
    kprintf("gui_installer: power off pressed\n");
    sleep_ms(60000);
    ktest_fail("the system did not power off");
}
KTEST_DEFINE("gui_installer", test_gui_installer);
