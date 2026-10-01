#include <arch/platform.h>
#include <arch/cpu.h>
#include <drivers/rtc.h>
#include <mm/vmm.h>
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

/* The PL031 real time clock of virt at 0x09010000: its data register
 * counts seconds since the Unix epoch. */
#define PL031_PHYS 0x09010000UL
#define RTCDR      0x00

/* The UTC calendar date of a day number counted from 1970-01-01. */
static void civil_from_days(int64_t z, struct rtc_date *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    d->day = (int)(doy - (153 * mp + 2) / 5 + 1);
    d->month = (int)(mp < 10 ? mp + 3 : mp - 9);
    d->year = (int)(yoe + era * 400 + (d->month <= 2));
}

void platform_rtc_read(struct rtc_date *d)
{
    volatile uint32_t *rtc = vmm_map_mmio(PL031_PHYS, 0x1000, VM_KERNEL_RW | VM_NOCACHE);
    if (!rtc) {
        *d = (struct rtc_date){ .year = 2000, .month = 1, .day = 1 };
        return;
    }
    int64_t seconds = rtc[RTCDR / 4];
    civil_from_days(seconds / 86400, d);
    int64_t rest = seconds % 86400;
    d->hour = (int)(rest / 3600);
    d->minute = (int)(rest / 60 % 60);
    d->second = (int)(rest % 60);
}

/* PCI arrives with the device tree and ECAM (A7). Until then every
 * configuration read finds no function, so the bus is empty. */
uint32_t platform_pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    return 0xffffffff;
}

void platform_pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v)
{
}

void platform_msi_compose(unsigned irq, uint64_t *addr, uint32_t *data)
{
    ARCH_TODO("A7");
}

/* The virt machine has no devices outside PCI and the device tree. */
void platform_devices_init(void)
{
}
