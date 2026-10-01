#include <arch/platform.h>
#include <arch/cpu.h>
#include "todo.h"

/* PSCI calls through hvc, the conduit QEMU uses for virt without EL2 or
 * EL3 firmware. */
#define PSCI_SYSTEM_OFF   0x84000008UL
#define PSCI_SYSTEM_RESET 0x84000009UL

static void psci_call(uint64_t fn)
{
    register uint64_t x0 __asm__("x0") = fn;
    __asm__ volatile("hvc #0" : "+r"(x0) : : "memory");
}

__noreturn void platform_power_off(void)
{
    psci_call(PSCI_SYSTEM_OFF);
    cpu_halt_forever();
}

__noreturn void platform_reboot(void)
{
    psci_call(PSCI_SYSTEM_RESET);
    cpu_halt_forever();
}

void platform_test_exit(int code)
{
    (void)code;
    psci_call(PSCI_SYSTEM_OFF);
}

void platform_rtc_read(struct rtc_date *d)
{
    (void)d;
    ARCH_TODO("A7");
}

uint32_t platform_pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    ARCH_TODO("A7");
}

void platform_pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v)
{
    ARCH_TODO("A7");
}

void platform_msi_compose(unsigned irq, uint64_t *addr, uint32_t *data)
{
    ARCH_TODO("A7");
}

/* The virt machine has no devices outside PCI and the device tree. */
void platform_devices_init(void)
{
}
