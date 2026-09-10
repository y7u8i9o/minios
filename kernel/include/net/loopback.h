#pragma once
/* The loopback interface lo (N02): output queues the buffer for the
 * worker as input on the same interface, so packets to the host itself
 * take the same path as packets from a device. Its MTU is the buffer
 * capacity below the headroom. */
#include <kernel.h>

#define LOOPBACK_MTU (PBUF_SIZE - PBUF_HEADROOM)

void loopback_init(void);
