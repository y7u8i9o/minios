#pragma once
#include <kernel.h>

struct input_dev;

/* Probe the mouse on the controller and register it (platform code,
 * arch/x86_64/i8042.c). */
void ps2mouse_init(void);
/* Register the input device for packets of packet_len bytes (3, or 4 with
 * a wheel); present records whether the mouse acknowledged the probe. */
void ps2mouse_register(unsigned packet_len, bool present);
/* Feed one byte from the auxiliary port (interrupt handler and tests). */
void ps2mouse_feed_byte(uint8_t b);
/* Whether the device sends four byte IntelliMouse packets. */
bool ps2mouse_has_wheel(void);
/* Select the packet length (tests). */
void ps2mouse_set_wheel(bool wheel);
/* The input device of the mouse (tests). */
struct input_dev *ps2mouse_device(void);
