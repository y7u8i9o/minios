#pragma once

/* Identification of the architecture (docs/design/arch.md). The machine
 * name is reported by uname; the ELF machine number is the only e_machine
 * that the ELF loader accepts. */
#define ARCH_MACHINE_NAME "x86_64"
#define ARCH_ELF_MACHINE  62    /* EM_X86_64 */
/* The interrupt model that the kernel reports to the AML code of ACPI
 * through \_PIC (uACPI UACPI_INTERRUPT_MODEL_IOAPIC). */
#define ARCH_ACPI_INTERRUPT_MODEL 1
