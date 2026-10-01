#pragma once
#include <kernel.h>

/* Map size bytes of device registers at physical address pa into the
 * kernel half before vmm_init, as Device memory. Returns NULL before the
 * direct map offset is known (boot_init). */
void *early_map_device(uintptr_t pa, size_t size);
