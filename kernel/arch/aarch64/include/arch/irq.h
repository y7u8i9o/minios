#pragma once
#include <kernel.h>

struct trapframe;
typedef void (*irq_handler_fn)(struct trapframe *tf, void *arg);

/* Interrupt numbers (docs/design/arch.md). On aarch64 an interrupt number
 * is the GIC interrupt ID: SGIs 0 to 15 are the IPIs, PPI 27 is the
 * virtual timer of each CPU. */
#define IRQ_TIMER         27
#define IRQ_RESCHED       1
#define IRQ_TLB_SHOOTDOWN 2
#define IRQ_HALT          3
#define IRQ_SPURIOUS      1023

void irq_register(unsigned irq, irq_handler_fn fn, void *arg);
void irq_dispatch(struct trapframe *tf);
int irq_alloc(void);
void arch_send_ipi(unsigned cpu, unsigned irq);
