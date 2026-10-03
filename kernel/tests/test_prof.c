/* Profiling scenario for the lock work (claude-lockfree branch): the
 * desktop with the panel, sysmon and the clock, pointer motion for
 * redraws, then prof over every process and the lock counters, all
 * printed on the console for analysis. */
#include <tests/ktest.h>
#include <sched/user.h>
#include <sched/proc.h>
#include <ipc/signal.h>
#include <drivers/timer.h>
#include <console.h>
#include "gui_helpers.h"

static struct proc *start(const char *path, const char *arg0)
{
    struct proc *p = proc_create_user(path, (char *const[]){ (char *)arg0, NULL }, (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(p != NULL, "cannot start %s", path);
    return p;
}

static void run_and_reap(const char *path, char *const argv[])
{
    struct proc *p = proc_create_user(path, argv, (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(p != NULL, "cannot start %s", path);
    int status = proc_reap(p);
    ktest_assert(status == 0, "%s status 0x%x", path, status);
}

static void test_prof_gui(void)
{
    install_app("mandel");
    uint64_t started = timer_ms();
    ktest_assert(fb_screen_present, "no framebuffer");
    struct proc *srv = start_server();
    struct proc *mon = start("/bin/sysmon", "sysmon");
    struct proc *clk = start("/bin/clock", "clock");
    struct proc *term = proc_create_user("/bin/term", (char *const[]){ "term", "yes", NULL }, (char *const[]){ NULL },
                                         &kernel_proc);
    ktest_assert(term != NULL, "cannot start the terminal");
    struct proc *mandel = start("/usr/bin/mandel", "mandel");
    sleep_ms(2000);
    /* Reset the lock counters after startup so they describe the steady
     * state, then generate pointer motion while sampling. */
    run_and_reap("/bin/sh", (char *const[]){ "sh", "-c", "echo reset > /dev/lockstat", NULL });
    struct proc *prof = proc_create_user("/bin/prof",
                                         (char *const[]){ "prof", "-e", "all", "-A", "512", "-c", "-g", "-t", "-m", "-i",
                                                          "-n", "40", "-d", "10", "-a", NULL },
                                         (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(prof != NULL, "cannot start prof");
    kprintf("prof_gui: input start at %lu ms\n", timer_ms() - started);
    int cx = logical_w() / 2, cy = logical_h() / 2;
    for (int i = 0; i < 420; i++) {
        mouse_move_to(&cx, &cy, 100 + (i % 10) * 40, 100 + (i % 7) * 30, 0);
        sleep_ms(20);
    }
    kprintf("prof_gui: input done at %lu ms\n", timer_ms() - started);
    int status = proc_reap(prof);
    kprintf("prof_gui: profiler reaped at %lu ms\n", timer_ms() - started);
    ktest_assert(status == 0, "prof status 0x%x", status);
    run_and_reap("/bin/cat", (char *const[]){ "cat", "/dev/lockstat", NULL });
    kprintf("prof_gui: lockstat done at %lu ms\n", timer_ms() - started);
    signal_send(mandel, SIGTERM);
    proc_reap(mandel);
    signal_send(term, SIGKILL);
    proc_reap(term);
    signal_send(clk, SIGTERM);
    proc_reap(clk);
    signal_send(mon, SIGTERM);
    proc_reap(mon);
    stop_server(srv);
    kprintf("prof_gui: done in %lu ms\n", timer_ms() - started);
}
KTEST_DEFINE("prof_gui", test_prof_gui);

/* The graphical profiler over a busy desktop: it records every class for
 * six seconds on its own and prints what it collected. */
static void test_profiler_gui(void)
{
    install_app("mandel");
    ktest_assert(fb_screen_present, "no framebuffer");
    struct proc *srv = start_server();
    struct proc *mandel = start("/usr/bin/mandel", "mandel");
    sleep_ms(1500);
    struct proc *gui = proc_create_user("/bin/profiler",
                                        (char *const[]){ "profiler", "-r", "6", "-o", "/tmp/profiler-gui.json", "--test-ui", NULL },
                                        (char *const[]){ NULL }, &kernel_proc);
    ktest_assert(gui != NULL, "cannot start the profiler");
    int cx = logical_w() / 2, cy = logical_h() / 2;
    for (int i = 0; i < 200; i++) {
        mouse_move_to(&cx, &cy, 120 + (i % 12) * 30, 120 + (i % 9) * 25, 0);
        sleep_ms(20);
    }
    sleep_ms(4000);
    signal_send(gui, SIGTERM);
    proc_reap(gui);
    signal_send(mandel, SIGTERM);
    proc_reap(mandel);
    stop_server(srv);
    kprintf("profiler_gui: done\n");
}
KTEST_DEFINE("profiler_gui", test_profiler_gui);
