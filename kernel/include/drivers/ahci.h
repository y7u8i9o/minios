#pragma once
/* AHCI host controllers (drivers/ahci.c, docs/design/ahci.md). */
#include <kernel.h>

/* Start every AHCI controller, register its ATA disks as sdX and its ATAPI
 * drives as srN. Called from kinit before the partition scan, since the
 * probe sleeps while it waits for the devices. */
void ahci_init(void);
