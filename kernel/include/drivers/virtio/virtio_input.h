#pragma once
#include <kernel.h>
#include <minios/input.h>

/* virtio-input: keyboards, mice and tablets become input core devices. */
void virtio_input_init(void);

/* The range QEMU's tablet reports and the virtual tablet uses. */
#define VIRTIO_INPUT_ABS_MAX 32767

/* Feed one event as if it came from the first pointer device, or from
 * the virtual tablet when none is attached (tests). */
void virtio_input_feed(uint16_t type, uint16_t code, uint32_t value);
