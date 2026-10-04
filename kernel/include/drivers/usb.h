#pragma once
#include <kernel.h>

/* USB (D2, docs/design/usb.md): the xHCI host controllers on PCI, the
 * enumeration of the devices on their ports and the HID class driver for
 * keyboards, mice and tablets.
 *
 * Find the xHCI controllers, start one thread for each and wait until each
 * thread has enumerated the devices connected at boot, at most two
 * seconds. Called from kinit after the input core and the PCI scan. */
void usb_init(void);

/* The number of devices that have an address on any controller (tests). */
unsigned usb_device_count(void);
