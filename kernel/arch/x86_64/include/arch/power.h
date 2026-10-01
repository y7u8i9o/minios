#pragma once
#include <kernel.h>

/* Platform power control (docs/design/arch.md).
 *
 * platform_power_off: unconditional power off through the QEMU q35 ACPI
 * PM1a control port, falling back to isa-debug-exit and finally to
 * halting. */
__noreturn void platform_power_off(void);
/* Reboot through the 8042 reset line, falling back to a triple fault. */
__noreturn void platform_reboot(void);
