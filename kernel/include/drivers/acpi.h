#pragma once
#include <kernel.h>

/* ACPI through uACPI with its AML interpreter (docs/design/acpi.md, V1 of
 * docs/plan/release-0.6.0.md). */

struct devinfo;

/* Load the namespace of the DSDT and the SSDTs, initialize the devices and
 * the events, and install the handlers of the power button. Called once in
 * kinit, after the PCI scan. Does nothing without ACPI tables. */
void acpi_init(void);
/* True after acpi_init has loaded the namespace. */
bool acpi_ready(void);
/* Enter the sleep state S5 with the values of \_S5 and the FADT. Returns
 * only when that fails, with interrupts disabled. */
void acpi_power_off(void);
/* Write the reset register of the FADT. Returns when the machine does not
 * restart or has no reset register. */
void acpi_reboot(void);
/* The power button of the machine: sends init the signal of shutdown.
 * Called in the context of a thread. */
void acpi_power_button(void);
/* The namespace and the power button, for /dev/devices. */
void acpi_describe(struct devinfo *d);
/* The reset method of ACPI for /dev/devices, or NULL without one. */
const char *acpi_reset_method(void);
/* Record the source of the power button that a driver outside ACPI
 * provides, such as the GPIO key of a device tree. */
void acpi_set_button_source(const char *source);

/* The kernel side of uACPI (drivers/acpi_kernel.c). */
/* Allow mappings outside the direct map, after vmm_init. */
void acpi_kernel_late(void);
/* Run fn(arg) in the ACPI work thread. Callable from an interrupt
 * handler. Returns false when the queue is full. */
bool acpi_defer(void (*fn)(void *arg), void *arg);
/* Start the thread of acpi_defer. */
void acpi_start_worker(void);
/* The IRQ_GSI_* flags of an interrupt that uACPI installs: the SCI of a PC
 * is an ISA interrupt that the MADT may override (drivers/acpi.c). */
unsigned acpi_irq_flags(unsigned gsi);
