#pragma once
/* Intel 82574L network controller (drivers/e1000e.c, docs/design/e1000e.md). */
#include <kernel.h>

/* Probe the first 82574L and register it as the next free ethN interface.
 * Called from net_init after virtio_net_init. */
void e1000e_init(void);
/* Reclaim completed transmits and pass received frames to the stack.
 * Registered as a network worker service and run on every worker pass. */
void e1000e_service(void);
/* Bring the interface down and reset the controller. Must run on the
 * network worker. Returns a negative errno value if the reset fails. */
int e1000e_stop(void);
bool e1000e_present(void);
/* Whether the MAC address was read from the EEPROM (for the boot test). */
bool e1000e_mac_from_eeprom(void);
bool e1000e_link_up(void);
