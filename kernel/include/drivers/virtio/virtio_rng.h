#pragma once
#include <kernel.h>
/* Boot-only bounded entropy acquisition. The caller receives 64 bytes only
 * after DMA has stopped. Failure never returns a partial seed. */
int virtio_rng_seed(uint8_t output[64]);
