#pragma once
#include <kernel.h>

/* Fill struct devtree from the ACPI tables (D1, docs/design/acpi.md).
 * Called by devtree_init when Limine passes no device tree. Returns false
 * without an RSDP or without a usable interrupt controller in the MADT. */
bool acpi_read_platform(void);
