/* The power key of a device tree (V1 of docs/plan/release-0.6.0.md): a
 * line of a PL061 GPIO controller, which QEMU virt raises for its power
 * button when it has no ACPI tables. The line interrupts on its rising
 * edge. The handler clears the interrupt of the line and defers the press
 * to the work thread of drivers/acpi.c. The registers are written by the
 * initialization before the interrupt is enabled and by the handler of
 * the interrupt afterwards, which runs on one CPU. */
#define KLOG_SUBSYS "powerkey"
#include <arch/platform.h>
#include <arch/irq.h>
#include <drivers/acpi.h>
#include <mm/vmm.h>
#include <klog.h>
#include "devtree.h"

#define GPIOIS  0x404           /* interrupt sense: 0 edge */
#define GPIOIBE 0x408           /* both edges */
#define GPIOIEV 0x40c           /* event: 1 rising edge */
#define GPIOIE  0x410           /* interrupt mask: 1 enabled */
#define GPIOMIS 0x418           /* masked interrupt status */
#define GPIOIC  0x41c           /* interrupt clear */

static volatile uint32_t *pl061;

static void press(void *arg)
{
    acpi_power_button();
}

static void key_interrupt(struct trapframe *tf, void *arg)
{
    uint32_t bit = 1u << devtree.power_line;
    uint32_t status = pl061[GPIOMIS / 4];
    pl061[GPIOIC / 4] = status;
    if (status & bit)
        acpi_defer(press, NULL);
}

void platform_power_key_init(void)
{
    if (devtree.from_acpi || !devtree.pl061 || devtree.power_line > 7)
        return;
    pl061 = vmm_map_mmio(devtree.pl061, 0x1000, VM_KERNEL_RW | VM_NOCACHE);
    if (!pl061)
        return;
    uint32_t bit = 1u << devtree.power_line;
    pl061[GPIOIE / 4] &= ~bit;
    pl061[GPIOIS / 4] &= ~bit;
    pl061[GPIOIBE / 4] &= ~bit;
    pl061[GPIOIEV / 4] |= bit;
    pl061[GPIOIC / 4] = bit;
    if (irq_route_gsi(devtree.pl061_irq, IRQ_GSI_LEVEL, key_interrupt, NULL) < 0) {
        klog_warn("cannot route interrupt %u", devtree.pl061_irq);
        return;
    }
    pl061[GPIOIE / 4] |= bit;
    acpi_set_button_source("the GPIO key of the device tree");
    klog_info("power key on line %u of the pl061 at %lx, interrupt %u", devtree.power_line,
              (unsigned long)devtree.pl061, devtree.pl061_irq);
}
