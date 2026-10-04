#include <arch/platform.h>
#include <arch/cpu.h>
#include <drivers/rtc.h>
#include <mm/vmm.h>
#include <drivers/pci.h>
#include <sync/spinlock.h>
#include "devtree.h"
#include "its.h"
#include "gic.h"

/* PSCI calls through the conduit of the device tree or the FADT: hvc on
 * QEMU virt without EL2 or EL3 firmware, smc where firmware at EL3
 * implements PSCI. */
#define PSCI_SYSTEM_OFF   0x84000008UL
#define PSCI_SYSTEM_RESET 0x84000009UL

static void psci_call(uint64_t fn)
{
    register uint64_t x0 __asm__("x0") = fn;
    if (devtree.psci_smc)
        __asm__ volatile("smc #0" : "+r"(x0) : : "memory");
    else
        __asm__ volatile("hvc #0" : "+r"(x0) : : "memory");
}

/* PSCI_VERSION: the major version in bits 31:16, the minor in 15:0. */
uint32_t platform_psci_version(void)
{
    register uint64_t x0 __asm__("x0") = 0x84000000UL;
    if (devtree.psci_smc)
        __asm__ volatile("smc #0" : "+r"(x0) : : "memory");
    else
        __asm__ volatile("hvc #0" : "+r"(x0) : : "memory");
    return (uint32_t)x0;
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

/* The PL031 real time clock (0x09010000 on virt): its data register counts
 * seconds since the Unix epoch. */
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
    volatile uint32_t *rtc = vmm_map_mmio(devtree.rtc, 0x1000, VM_KERNEL_RW | VM_NOCACHE);
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

/* PCIe configuration through the ECAM window of the device tree: 4 KiB per
 * function, 1 MiB per bus, counted from the first bus of the window. A
 * bus is mapped at its first access. Reads outside the window find no
 * function. */
static DEFINE_SPINLOCK(ecam_lock);      /* protects ecam_bus */
static volatile uint8_t *ecam_bus[256];

static volatile uint32_t *ecam_config(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    if (!devtree.ecam || bus < devtree.bus_start || bus > devtree.bus_end)
        return NULL;
    spin_lock(&ecam_lock);
    volatile uint8_t *base = ecam_bus[bus];
    if (!base) {
        uintptr_t pa = devtree.ecam + ((uintptr_t)(bus - devtree.bus_start) << 20);
        base = vmm_map_mmio(pa, 1UL << 20, VM_KERNEL_RW | VM_NOCACHE);
        ecam_bus[bus] = base;
    }
    spin_unlock(&ecam_lock);
    if (!base)
        return NULL;
    return (volatile uint32_t *)(base + ((uintptr_t)slot << 15) + ((uintptr_t)func << 12) + (off & 0xfc));
}

uint32_t platform_pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    volatile uint32_t *reg = ecam_config(bus, slot, func, off);
    return reg ? *reg : 0xffffffff;
}

void platform_pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v)
{
    volatile uint32_t *reg = ecam_config(bus, slot, func, off);
    if (reg)
        *reg = v;
}

/* MSI goes through the ITS on a GICv3 (its.c), where the device ID is the
 * requester ID translated by the msi-map of the device tree, and through
 * the v2m frame on a GICv2, where the interrupt goes to the boot CPU. */
void platform_msi_compose(const struct pci_dev *dev, unsigned irq, unsigned cpu,
                          uint64_t *addr, uint32_t *data)
{
    if (devtree.gic_version == 2) {
        gic_v2m_msi_compose(irq, addr, data);
        return;
    }
    uint32_t rid = (uint32_t)dev->bus << 8 | (uint32_t)dev->slot << 3 | dev->func;
    its_msi_compose(rid - devtree.msi_rid_base + devtree.msi_base, irq, cpu, addr, data);
}

/* The virt machine has no devices outside PCI and the device tree. */
void platform_devices_init(void)
{
}
