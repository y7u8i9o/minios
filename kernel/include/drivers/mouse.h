#pragma once
#include <kernel.h>
#include <minios/abi.h>

/* /dev/mouse: a ring of fixed size events fed by the PS/2 mouse and
 * virtio-input drivers. */
void mouse_init(void);
/* Queue one event (time_ms is filled in). Callable from interrupt
 * handlers and from virtqueue completion callbacks. */
void mouse_push(const struct mouse_event *e);
