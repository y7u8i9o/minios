#include <arch/platform.h>
#include <arch/io.h>
#include <arch/cpu.h>

#define QEMU_Q35_PM1A_CNT   0x604
#define ACPI_SLP_TYP_S5     0x2000

__noreturn void platform_power_off(void)
{
    cli();
    outw(QEMU_Q35_PM1A_CNT, ACPI_SLP_TYP_S5);
    /* The power off completes asynchronously; give it time before the
     * fallback, which would otherwise win the race with a different exit
     * status. */
    for (volatile long i = 0; i < 200000000L; i++)
        cpu_relax();
    /* Not on a q35 with ACPI, or the write was ignored: try the test device. */
    platform_test_exit(0);
    cpu_halt_forever();
}

__noreturn void platform_reboot(void)
{
    cli();
    /* Wait for the 8042 input buffer to drain, then pulse the reset line. */
    for (int i = 0; i < 100000; i++) {
        if (!(inb(0x64) & 0x02))
            break;
    }
    outb(0x64, 0xfe);
    for (volatile int i = 0; i < 1000000; i++)
        cpu_relax();

    /* Fallback: load an empty IDT and fault, which triple faults the CPU. */
    struct {
        uint16_t limit;
        uint64_t base;
    } __packed empty = { 0, 0 };
    __asm__ volatile("lidt %0; int3" : : "m"(empty));
    cpu_halt_forever();
}
