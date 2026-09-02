#pragma once
#include <kernel.h>

/* virtio-gpu (QEMU virtio-vga): takes over the display at boot with a
 * guest memory scanout buffer, flushes damaged rectangles and changes
 * the mode at run time. Registers itself with drivers/fbdev.h. */
void virtio_gpu_init(void);
bool virtio_gpu_present(void);
