#pragma once
#include <kernel.h>

void ps2mouse_init(void);
/* Feed one byte from the auxiliary port (interrupt handler and tests). */
void ps2mouse_feed_byte(uint8_t b);
/* Whether the device sends four byte IntelliMouse packets. */
bool ps2mouse_has_wheel(void);
/* Select the packet length (tests). */
void ps2mouse_set_wheel(bool wheel);
