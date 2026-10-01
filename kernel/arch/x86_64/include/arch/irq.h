#pragma once
#include <kernel.h>

struct trapframe;
typedef void (*irq_handler_fn)(struct trapframe *tf, void *arg);

/* Register a handler for a vector at or above IRQ_VECTOR_BASE. The trap
 * dispatcher calls it with interrupts disabled and sends the EOI afterwards. */
void irq_register(uint8_t vector, irq_handler_fn fn, void *arg);
void irq_dispatch(struct trapframe *tf);
