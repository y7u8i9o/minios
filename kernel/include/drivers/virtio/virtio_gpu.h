#pragma once
#include <kernel.h>

/* virtio-gpu (QEMU virtio-vga): takes over the display at boot with a
 * guest memory scanout buffer, flushes damaged rectangles and changes
 * the mode at run time. Registers itself with drivers/fbdev.h. */
void virtio_gpu_init(void);
bool virtio_gpu_present(void);
/* The position of the last cursor request: the top left corner of the
 * cursor image, the hotspot position minus the hotspot (tests). */
void virtio_gpu_cursor_sent(int32_t *x, int32_t *y);
