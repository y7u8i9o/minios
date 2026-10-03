#pragma once
/* Futexes: user space waits on a word of its own address space and
 * wakes waiters of the same word (M35). Private to one process. */
#include <kernel.h>

void futex_init(void);
/* Sleep while the word at uaddr contains value; timeout_ms 0 is unlimited.
 * Returns 0 when woken, -EAGAIN when the value differs, -ETIMEDOUT or
 * -EINTR. */
int futex_wait(uintptr_t uaddr, uint32_t value, uint64_t timeout_ms);
/* Wake up to count waiters of uaddr; returns how many were woken. */
int futex_wake(uintptr_t uaddr, uint32_t count);
