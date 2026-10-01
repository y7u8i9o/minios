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

__noreturn void ktest_pass(void)
{
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
