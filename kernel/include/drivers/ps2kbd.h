#pragma once
#include <kernel.h>

struct input_dev;

/* Software key repeat of the PS/2 keyboard (EVIOCSREP changes it). */
#define PS2KBD_REPEAT_DELAY_MS  500
#define PS2KBD_REPEAT_PERIOD_MS 33

void ps2kbd_init(void);
/* Translate one scancode (set 1) and report the key to the input core.
 * Called by the interrupt handler and by tests. */
void ps2kbd_feed_scancode(uint8_t code);
/* The input device of the keyboard (tests). */
struct input_dev *ps2kbd_device(void);
