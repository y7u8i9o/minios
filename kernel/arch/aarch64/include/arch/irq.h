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
/* Route a global system interrupt of the firmware, such as the SCI of ACPI
 * or a line of the device tree, to fn and enable it. flags are
 * IRQ_GSI_LEVEL or edge triggered, and IRQ_GSI_ACTIVE_LOW or active high.
 * Returns the interrupt number or a negative errno value. */
#define IRQ_GSI_LEVEL      1
#define IRQ_GSI_ACTIVE_LOW 2
int irq_route_gsi(unsigned gsi, unsigned flags, irq_handler_fn fn, void *arg);
void irq_dispatch(struct trapframe *tf);
int irq_alloc(void);
void arch_send_ipi(unsigned cpu, unsigned irq);
