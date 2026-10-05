#pragma once
#include <kernel.h>

/* Platform services that generic code calls (docs/design/arch.md),
 * implemented by the architecture for its QEMU machine: the q35 PC on
 * x86_64, virt on aarch64. The console UART implements
 * <drivers/serial.h>. */

struct rtc_date;
struct pci_dev;

/* Unconditional power off (sleep state S5 of ACPI, PSCI SYSTEM_OFF on
 * virt), halting if that fails. */
__noreturn void platform_power_off(void);
/* Reboot (the reset register of ACPI or the 8042 reset line, PSCI
 * SYSTEM_RESET on virt). */
__noreturn void platform_reboot(void);

/* I/O ports of the width 1, 2 or 4 bytes, for the ACPI interpreter.
 * platform_has_ports is false on an architecture without an I/O port
 * space, where the other two functions are not called. */
bool platform_has_ports(void);
uint32_t platform_port_read(uint16_t port, unsigned width);
void platform_port_write(uint16_t port, unsigned width, uint32_t value);
/* End a QEMU test run with an exit status. On the PC the isa-debug-exit
 * device on port 0xf4 makes QEMU exit with (code << 1) | 1 and the call
 * returns if the device is not present. On virt QEMU powers off, without
 * the status. */
void platform_test_exit(int code);

/* Read the battery backed clock as a UTC calendar date. */
void platform_rtc_read(struct rtc_date *d);

/* 32 bit read and write of the PCI configuration space of one function.
 * off is a multiple of 4. */
uint32_t platform_pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off);
void platform_pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v);
/* The MSI address and data with which dev delivers interrupt number irq
 * to the CPU with the kernel id cpu, which has started. On aarch64 this
 * also maps the message in the ITS, which identifies the sender by its
 * requester ID. */
void platform_msi_compose(const struct pci_dev *dev, unsigned irq, unsigned cpu,
                          uint64_t *addr, uint32_t *data);

/* Register the devices that only this platform has (the PS/2 keyboard and
 * mouse on the PC). Called after the input core is initialized. */
void platform_devices_init(void);
/* Install a power button that ACPI does not provide, such as the GPIO key
 * of a device tree. Called in kinit after acpi_init. */
void platform_power_key_init(void);
