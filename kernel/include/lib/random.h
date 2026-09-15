#pragma once
#include <kernel.h>
/* One boot seed from VirtIO entropy. Absence/failure leaves the provider
 * unavailable. No timer, fixed seed, libc PRNG or CPU fallback is used. */
void random_init(void);
bool random_ready(void);
int random_u32(uint32_t *value);
