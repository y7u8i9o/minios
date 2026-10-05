#pragma once

/* Identification of the architecture (docs/design/arch.md). */
#define ARCH_MACHINE_NAME "aarch64"
#define ARCH_ELF_MACHINE  183   /* EM_AARCH64 */
/* The interrupt model that the kernel reports to the AML code of ACPI
 * through \_PIC (uACPI UACPI_INTERRUPT_MODEL_GIC). */
#define ARCH_ACPI_INTERRUPT_MODEL 4
