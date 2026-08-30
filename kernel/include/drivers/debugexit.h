#pragma once
#include <kernel.h>

/* Exit QEMU through the isa-debug-exit device on port 0xf4. QEMU exits with
 * status (code << 1) | 1. Returns if the device is not present. */
void debugexit_exit(int code);
