#pragma once
/* Event and timer descriptors (M23). */
#include <kernel.h>
#include <fs/vfs.h>

int eventfd_create(uint64_t initval, int flags, struct file **out);
int timerfd_create(int flags, struct file **out);
int timerfd_settime(struct file *f, uint64_t initial_ms, uint64_t interval_ms);
int timerfd_gettime(struct file *f, uint64_t *remaining_ms, uint64_t *interval_ms);
/* Called from the timer interrupt on the boot CPU every tick. */
void timerfd_tick(void);
