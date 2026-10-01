#include <tests/ktest.h>
#include <cpu.h>
#include <arch/cpu.h>
#include <arch/irq.h>
#include <arch/timer.h>
#include <arch/platform.h>
#include <arch/smp.h>
#include <drivers/pci.h>
#include <drivers/rtc.h>
#include <drivers/timer.h>
#include <console.h>

/* A1: the platform interface (docs/design/arch.md). */

static void test_platform(void)
{
    /* Per CPU structures. */
    struct cpu *c = cpu_current();
    ktest_assert(c == cpu_by_id(c->id), "cpu_current is not cpu_by_id(%u)", c->id);
    ktest_assert(c->self == c, "self pointer of cpu %u", c->id);
    for (unsigned i = 0; i < smp_cpu_count(); i++)
        ktest_assert(cpu_by_id(i)->id == i, "cpu_by_id(%u) has id %u", i, cpu_by_id(i)->id);

    /* The clock advances and agrees with timer_ns. */
    uint64_t c0 = arch_clock_read();
    uint64_t n0 = timer_ns();
    sleep_ms(20);
    uint64_t c1 = arch_clock_read();
    uint64_t n1 = timer_ns();
    ktest_assert(c1 > c0, "clock did not advance");
    ktest_assert(n1 - n0 >= 20000000 && n1 - n0 < 2000000000, "20 ms sleep measured as %lu ns", n1 - n0);

    /* Two allocated interrupt numbers differ and both deliver through MSI,
     * here composed for the host bridge. */
    int a = irq_alloc(), b = irq_alloc();
    ktest_assert(a >= 0 && b >= 0 && a != b, "irq_alloc returned %d and %d", a, b);
    ktest_assert(pci_count() > 0, "no PCI functions");
    uint64_t addr;
    uint32_t data;
    platform_msi_compose(pci_device(0), (unsigned)a, &addr, &data);
    ktest_assert(addr != 0, "MSI address 0 for irq %d", a);

    /* The clock device reads a plausible date. */
    struct rtc_date d;
    platform_rtc_read(&d);
    ktest_assert(d.year >= 2020 && d.month >= 1 && d.month <= 12 && d.day >= 1 && d.day <= 31,
                 "rtc date %04d-%02d-%02d", d.year, d.month, d.day);

    /* PCI function 0 of slot 0 on bus 0 is the host bridge. */
    uint32_t id = platform_pci_read32(0, 0, 0, 0);
    ktest_assert((id & 0xffff) != 0xffff, "no host bridge at 00:00.0 (id %08x)", id);

    kprintf("platform: %u cpus, irqs %d and %d, MSI %lx/%x, host bridge %04x:%04x\n",
            smp_cpu_count(), a, b, addr, data, id & 0xffff, id >> 16);
}
KTEST_DEFINE("platform", test_platform);
