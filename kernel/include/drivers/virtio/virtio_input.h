#pragma once
#include <kernel.h>

/* virtio-input: tablets (absolute position) and mice feed /dev/mouse. */
void virtio_input_init(void);

/* Linux event codes used by the protocol. */
#define EV_SYN 0
#define EV_KEY 1
#define EV_REL 2
#define EV_ABS 3
#define SYN_REPORT 0
#define BTN_LEFT   0x110
#define BTN_RIGHT  0x111
#define BTN_MIDDLE 0x112
#define REL_X      0
#define REL_Y      1
#define REL_WHEEL  8
#define ABS_X      0
#define ABS_Y      1

/* Feed one event as if it came from the first device, or from a virtual
 * tablet when none is present (tests). */
void virtio_input_feed(uint16_t type, uint16_t code, uint32_t value);
