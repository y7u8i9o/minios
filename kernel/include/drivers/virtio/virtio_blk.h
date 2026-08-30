#pragma once
#include <kernel.h>

/* Probe every virtio-blk PCI function and register block devices vda,
 * vdb, ... */
void virtio_blk_init(void);
