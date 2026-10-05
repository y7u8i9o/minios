/* V1 of docs/plan/release-0.6.0.md: ACPI power off, reset and power button.
 *
 * acpi_power starts init and reports that it waits. The QMP script of the
 * case then presses the power button of QEMU, and init must shut the
 * machine down in order. The case runs on x86_64, where the button is the
 * fixed power button event, on aarch64 with a device tree, where it is the
 * GPIO key of the PL061, and on aarch64 with ACPI (acpi_power_acpi), where
 * it reaches the power button device through the Generic Event Device.
 *
 * acpi_reset writes the reset register of the FADT. The QMP script waits
 * for the event SHUTDOWN, which QEMU sends with the reason guest-reset
 * because the case runs with -no-reboot.
 *
 * acpi_s5 evaluates \_S5 on a machine with the fixed hardware of ACPI.
 * The hardware reduced ACPI of aarch64 has no \_S5. There the test checks
 * that the FADT requires PSCI. */
#include <tests/ktest.h>
#include <drivers/acpi.h>
#include <sched/proc.h>
#include <console.h>
#include <uacpi/uacpi.h>
#include <uacpi/types.h>
#include <uacpi/tables.h>
#include <uacpi/acpi.h>

struct proc *ktest_start_init(void);

static void test_acpi_power(void)
{
    struct proc *p = ktest_start_init();
    ktest_wait_idle(5000);
    kprintf("acpi_power: ready for the power button\n");
    int status = proc_reap(p);
    ktest_fail("init exited with status 0x%x", status);
}
KTEST_DEFINE("acpi_power", test_acpi_power);

static void test_acpi_reset(void)
{
    ktest_assert(acpi_ready(), "the ACPI namespace is not loaded");
    ktest_assert(acpi_reset_method() != NULL, "the FADT has no reset register");
    kprintf("acpi_reset: writing the reset register\n");
    /* The console drains its queue in a thread, which the reset stops. */
    console_flush();
    acpi_reboot();
    ktest_fail("the reset register had no effect");
}
KTEST_DEFINE("acpi_reset", test_acpi_reset);

static void test_acpi_s5(void)
{
    ktest_assert(acpi_ready(), "the ACPI namespace is not loaded");
    uacpi_bool reduced = UACPI_FALSE;
    uacpi_is_platform_reduced_hardware(&reduced);
    if (reduced) {
        /* Hardware reduced ACPI has no PM1 registers. The FADT of an ARM
         * machine then requires PSCI for the power off and the reset. */
        struct acpi_fadt *fadt;
        ktest_assert(uacpi_table_fadt(&fadt) == UACPI_STATUS_OK, "no FADT");
        ktest_assert(fadt->arm_boot_arch & ACPI_ARM_PSCI_COMPLIANT, "the FADT does not require PSCI");
        kprintf("acpi_s5: reduced hardware, power off through PSCI\n");
        kprintf("acpi_s5: ok\n");
        return;
    }
    uacpi_object *s5;
    uacpi_status st = uacpi_eval_simple_package(NULL, "\\_S5", &s5);
    ktest_assert(st == UACPI_STATUS_OK, "\\_S5: %s", uacpi_status_to_string(st));
    uacpi_object_array values;
    ktest_assert(uacpi_object_get_package(s5, &values) == UACPI_STATUS_OK && values.count >= 1,
                 "\\_S5 is no package with values");
    uint64_t typ_a = 0;
    ktest_assert(uacpi_object_get_integer(values.objects[0], &typ_a) == UACPI_STATUS_OK, "SLP_TYPa is no integer");
    kprintf("acpi_s5: \\_S5 has %u values, SLP_TYPa %lu\n", (unsigned)values.count, (unsigned long)typ_a);
    uacpi_object_unref(s5);
    kprintf("acpi_s5: ok\n");
}
KTEST_DEFINE("acpi_s5", test_acpi_s5);
