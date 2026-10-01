#pragma once
#include <debug/panic.h>

/* An interface function whose aarch64 implementation belongs to a later
 * milestone of docs/plan/arm64.md. Reaching it stops the kernel with the
 * function and the milestone named. */
#define ARCH_TODO(milestone) \
    panic("aarch64: %s is not implemented before milestone %s", __func__, milestone)
