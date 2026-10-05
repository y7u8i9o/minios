#define KLOG_SUBSYS "ktest"
#include <tests/ktest.h>
#include <lib/cmdline.h>
#include <lib/string.h>
#include <console.h>
#include <klog.h>
#include <arch/cpu.h>
#include <arch/platform.h>
#include <drivers/ps2kbd.h>
#include <drivers/ps2mouse.h>
#include <input/input.h>
#include <drivers/timer.h>
#include <sched/sched.h>
#include <sched/wait.h>
#include <block/blockdev.h>
#include <ipc/eventfd.h>

extern const struct ktest __ktests_start[], __ktests_end[];

void ktest_run_stage(enum ktest_stage stage)
{
    static const char *const names[] = {
        [KTEST_KINIT] = "", [KTEST_EARLY] = "early ", [KTEST_MEMORY] = "memory stage ",
        [KTEST_TIMER] = "timer stage ",
    };
    char name[64];
    if (!cmdline_lookup("test", name, sizeof name))
        return;
    for (const struct ktest *t = __ktests_start; t < __ktests_end; t++) {
        if (t->stage == stage && strcmp(t->name, name) == 0) {
            klog_info("running %stest %s", names[stage], name);
            if (stage == KTEST_TIMER)
                arch_irq_enable();
            t->fn();
            ktest_pass();
        }
    }
}

/* The boot tests inject keys and pointer motion through the PS/2
 * decoders, which the PC registers for its 8042 controller. A platform
 * without one (virt on aarch64) gets the decoders as input devices for the
 * test run, with the wheel that QEMU's PS/2 mouse reports. */
static void attach_test_input(void)
{
    if (!input_device_by_name("AT Translated Set 2 keyboard"))
        ps2kbd_register();
    if (!input_device_by_name("ImPS/2 Generic Wheel Mouse") && !input_device_by_name("PS/2 Generic Mouse"))
        ps2mouse_register(4, true);
}

void ktest_run_selected(void)
{
    char name[64];
    if (!cmdline_lookup("test", name, sizeof name))
        return;
    attach_test_input();

    for (const struct ktest *t = __ktests_start; t < __ktests_end; t++) {
        if (t->stage == KTEST_KINIT && strcmp(t->name, name) == 0) {
            klog_info("running test %s", name);
            t->fn();
            ktest_pass();
        }
    }
    ktest_fail("unknown test %s", name);
}

/* The statistics of ktest_wait_idle for the line that ktest_pass prints.
 * Only the test thread writes them. */
static unsigned idle_waits, idle_limits;
static uint64_t idle_waited_ms, idle_limit_ms;

__noreturn void ktest_pass(void)
{
    if (idle_waits)
        kprintf("ktest: %u idle waits, %u reached their limit, %llu of %llu ms waited\n", idle_waits, idle_limits,
                (unsigned long long)idle_waited_ms, (unsigned long long)idle_limit_ms);
    kprintf("TEST PASS\n");
    console_flush();
    platform_test_exit(0);
    cpu_halt_forever();
}

__noreturn void ktest_fail(const char *fmt, ...)
{
    kprintf("TEST FAIL ");
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    kprintf("\n");
    console_flush();
    platform_test_exit(1);
    cpu_halt_forever();
}

/* QUIET_SAMPLES samples QUIET_GAP_MS apart must find the system quiet.  A
 * single sample can fall between the wakeup of a thread and its entry into
 * a run queue.  A thread that waits for a device, such as a disk read,
 * looks idle, and the samples over 6 ms cover the time of such a request
 * in QEMU. */
#define QUIET_SAMPLES 4
#define QUIET_GAP_MS 2

void ktest_wait_idle(int max_ms)
{
    struct thread *self = cpu_current()->current;
    uint64_t start = timer_ms();
    /* A timer of a user process that expires inside the wait counts as
     * pending work, because the fixed wait would have seen its effect. An
     * armed timerfd counts as well. X12 composes the damage of its clients
     * only when its frame timerfd expires. */
    uint64_t until_ms = start + (uint64_t)max_ms;
    uint64_t until_tick = timer_ticks() + (uint64_t)max_ms * TIMER_HZ / 1000;
    int quiet = 0;
    idle_waits++;
    idle_limit_ms += (uint64_t)max_ms;
    for (;;) {
        bool now_quiet = sched_quiet(self, until_tick) && !blockdev_busy() &&
                         waitq_next_user_deadline() > until_ms && timerfd_next_deadline() > until_ms;
        quiet = now_quiet ? quiet + 1 : 0;
        uint64_t waited = timer_ms() - start;
        if (quiet == QUIET_SAMPLES || waited >= (uint64_t)max_ms) {
            if (quiet != QUIET_SAMPLES)
                idle_limits++;
            idle_waited_ms += waited;
            return;
        }
        sleep_ms(QUIET_GAP_MS);
    }
}
