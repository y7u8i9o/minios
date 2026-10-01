#define KLOG_SUBSYS "ktest"
#include <tests/ktest.h>
#include <lib/cmdline.h>
#include <lib/string.h>
#include <console.h>
#include <klog.h>
#include <arch/cpu.h>
#include <arch/platform.h>

extern const struct ktest __ktests_start[], __ktests_end[];

void ktest_run_early(void)
{
    char name[64];
    if (!cmdline_lookup("test", name, sizeof name))
        return;
    for (const struct ktest *t = __ktests_start; t < __ktests_end; t++) {
        if (t->early && strcmp(t->name, name) == 0) {
            klog_info("running early test %s", name);
            t->fn();
            ktest_pass();
        }
    }
}

void ktest_run_selected(void)
{
    char name[64];
    if (!cmdline_lookup("test", name, sizeof name))
        return;

    for (const struct ktest *t = __ktests_start; t < __ktests_end; t++) {
        if (strcmp(t->name, name) == 0) {
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
