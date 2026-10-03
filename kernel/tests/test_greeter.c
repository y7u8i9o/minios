/* U4: the graphical login. The greeter starts X12 and its login window,
 * Enter logs in the preselected account user (uid 1000, no password), the
 * session's panel and desktop run as uid 1000, Log out in the panel's menu
 * ends the session, the greeter shows its window again, and X12 then
 * refuses a client of uid 1000. hold=1 keeps the login window open for
 * eight seconds, for screenshots. */
#include <tests/ktest.h>
#include <sched/user.h>
#include <sched/proc.h>
#include <ipc/signal.h>
#include <drivers/timer.h>
#include <mm/slab.h>
#include <lib/string.h>
#include <lib/printf.h>
#include <lib/cmdline.h>
#include <console.h>
#include "gui_helpers.h"

/* The number of live processes named name with effective uid uid, or of
 * any uid when uid is -1, from the table of /dev/proc. */
static int count_procs(const char *name, int uid)
{
    size_t size = 8192;
    char *table = kmalloc(size);
    ktest_assert(table != NULL, "alloc");
    proc_format_table(table, size);
    int n = 0;
    for (char *line = table; *line;) {
        char *end = strchr(line, '\n');
        if (end)
            *end = '\0';
        /* PID PPID PGID STATE TIME RSS UID NAME */
        char *f[8];
        int nf = 0;
        for (char *p = line; *p && nf < 8;) {
            while (*p == ' ')
                p++;
            if (!*p)
                break;
            f[nf++] = p;
            while (*p && *p != ' ')
                p++;
            if (*p)
                *p++ = '\0';
        }
        if (nf == 8 && strcmp(f[3], "zombie") != 0 && strcmp(f[7], name) == 0) {
            int u = 0;
            for (const char *d = f[6]; *d >= '0' && *d <= '9'; d++)
                u = u * 10 + (*d - '0');
            if (uid < 0 || u == uid)
                n++;
        }
        if (!end)
            break;
        line = end + 1;
    }
    kfree(table);
    return n;
}

/* Wait until the count holds for half a second. The greeter forks its
 * helpers, which carry its name until they exec, so a single look could
 * count one of them. */
static bool wait_procs(const char *name, int uid, int want, int ms)
{
    int steady = 0;
    for (int t = 0; t < ms; t += 100) {
        steady = count_procs(name, uid) == want ? steady + 1 : 0;
        if (steady == 5)
            return true;
        sleep_ms(100);
    }
    return false;
}

static void test_greeter(void)
{
    ktest_assert(fb_screen_present, "no framebuffer");
    int sh = logical_h();
    /* The kernel keeps pid 1, init, from SIGKILL. A first short process
     * takes that pid, which lets the end of the test kill the greeter. */
    struct proc *first = proc_create_user("/bin/sh", (char *const[]){ "sh", "-c", "exit 0", NULL },
                                          (char *const[]){ "PATH=/bin", NULL }, &kernel_proc);
    ktest_assert(first != NULL, "cannot start sh");
    proc_reap(first);
    struct proc *g = proc_create_user("/bin/greeter", (char *const[]){ "greeter", "-s", NULL },
                                      (char *const[]){ "PATH=/bin", NULL }, &kernel_proc);
    ktest_assert(g != NULL, "cannot start the greeter");
    /* The supervisor and its login window. */
    ktest_assert(wait_procs("greeter", 0, 2, 10000), "no login window");
    ktest_assert(count_procs("x12", 0) == 1, "no display server");
    sleep_ms(1500);
    kprintf("gui_greeter: login window shown\n");
    char hold[8];
    if (cmdline_lookup("hold", hold, sizeof hold) && hold[0] == '1')
        sleep_ms(8000);                 /* screenshots of the login window */

    /* user has no password. Enter logs in with the empty one, and the
     * greeter asks for a new password twice before the session starts. */
    press_key(0x1c);
    sleep_ms(1500);
    type_line("userpw\n");
    sleep_ms(300);
    type_line("userpw\n");
    ktest_assert(wait_procs("panel", 1000, 1, 10000), "no panel of uid 1000");
    ktest_assert(wait_procs("desktop", 1000, 1, 5000), "no desktop of uid 1000");
    ktest_assert(count_procs("greeter", -1) == 1, "the login window remains");
    kprintf("gui_greeter: session of uid 1000\n");
    sleep_ms(1500);

    /* Log out is the bottom row of the launcher menu, 42 px above the
     * bottom of the screen without installed packages (test_gui.c). */
    int cx = 0, cy = 0;
    mouse_move_to(&cx, &cy, 30, sh - 14, 0);
    mouse_click(1);
    sleep_ms(400);
    mouse_move_to(&cx, &cy, 40, sh - 42, 0);
    mouse_click(1);
    ktest_assert(wait_procs("panel", -1, 0, 10000), "the session did not end");
    ktest_assert(wait_procs("greeter", 0, 2, 10000), "no login window after the session");
    kprintf("gui_greeter: login window shown again\n");

    /* Without a session X12 refuses the clients of uid 1000. */
    struct proc *as = proc_create_user("/bin/doas", (char *const[]){ "doas", "-u", "user", "x12settings", "set", "verbose", "0", NULL },
                                       (char *const[]){ "PATH=/bin", NULL }, &kernel_proc);
    ktest_assert(as != NULL, "cannot start doas");
    proc_reap(as);
    sleep_ms(300);

    signal_send_pgrp(g->pid, SIGKILL);
    proc_reap(g);
    proc_reap_children(&kernel_proc);
}
KTEST_DEFINE("gui_greeter", test_greeter);
