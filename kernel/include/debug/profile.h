#pragma once
#include <kernel.h>

struct trapframe;

/* Sampling profiler (M41): the timer interrupt records the interrupted
 * instruction pointer and a frame pointer chain into a ring that
 * /dev/profile hands to user space. See docs/design/profile.md. */
void profile_init(void);
/* Called from the timer interrupt of every CPU with interrupts disabled. */
void profile_sample(const struct trapframe *tf);
