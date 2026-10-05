#pragma once
/* NVMe controllers (drivers/nvme.c, docs/design/nvme.md). */
#include <kernel.h>

/* Start every NVMe controller and register its namespaces as the block
 * devices nvmeCnN. Called from kinit before the partition scan, since the
 * probe sleeps while it waits for admin commands. */
void nvme_init(void);
