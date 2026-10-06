#pragma once
#include <sys/types.h>
#include <minios/abi.h>

/* Up to 256 random bytes of the kernel generator per call. Returns the
 * number of bytes, or -1 with errno EAGAIN when the kernel has no boot
 * seed (no virtio-rng). flags: GRND_NONBLOCK, GRND_RANDOM; both change
 * nothing, because the generator never blocks. */
ssize_t getrandom(void *buf, size_t len, unsigned flags);
