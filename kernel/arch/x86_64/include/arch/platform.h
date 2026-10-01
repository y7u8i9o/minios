#pragma once
#include <kernel.h>

/* Platform services of the QEMU q35 PC that generic code calls
 * (docs/design/arch.md). The console UART implements <drivers/serial.h>;
 * the PS/2 keyboard and mouse (<drivers/ps2kbd.h>, <drivers/ps2mouse.h>)
 * exist only on this platform. */

struct rtc_date;

/* Unconditional power off through the QEMU q35 ACPI PM1a control port,
 * falling back to isa-debug-exit and finally to halting. */
__noreturn void platform_power_off(void);
/* Reboot through the 8042 reset line, falling back to a triple fault. */
__noreturn void platform_reboot(void);
/* End a QEMU test run with an exit status: the isa-debug-exit device on
 * port 0xf4, which makes QEMU exit with (code << 1) | 1. Returns if the
 * device is not present. */
void platform_test_exit(int code);

/* Read the battery backed clock (CMOS) as a UTC calendar date. */
void platform_rtc_read(struct rtc_date *d);

/* PCI configuration space of one function, through configuration
 * mechanism 1 (ports 0xcf8 and 0xcfc). off is a multiple of 4. */
uint32_t platform_pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
void platform_pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v);
/* The MSI address and data that deliver interrupt number irq to the
 * calling CPU. */
void platform_msi_compose(unsigned irq, uint64_t *addr, uint32_t *data);

/* Register the devices that only this platform has: the PS/2 keyboard and
 * mouse. Called after the input core is initialized. */
void platform_devices_init(void);
