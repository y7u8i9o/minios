#pragma once
#include <kernel.h>

struct trapframe;

/* Set once a panic has begun so that the console stops taking its lock. */
extern volatile int panic_in_progress;

__noreturn void panic(const char *fmt, ...) __printf(1, 2);
/* Panic from an exception handler: dumps the trap frame and walks its
 * frame pointer chain before the usual panic output. */
__noreturn void panic_trap(struct trapframe *tf, const char *fmt, ...) __printf(2, 3);
