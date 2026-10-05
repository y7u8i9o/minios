#pragma once
#include <kernel.h>

struct trapframe;
typedef void (*irq_handler_fn)(struct trapframe *tf, void *arg);

/* Interrupt numbers (docs/design/arch.md). On x86_64 an interrupt number
 * is the IDT vector. Generic code uses the named numbers below and
 * irq_alloc; the PC devices use the fixed vectors of <arch/apic.h>. */
#define IRQ_VECTOR_BASE   32
#define IRQ_TIMER         32    /* the local tick of every CPU */
/* Inter processor interrupts. */
#define IRQ_TLB_SHOOTDOWN 0xf0
#define IRQ_HALT          0xf1
#define IRQ_RESCHED       0xf2
#define IRQ_SPURIOUS      0xff

/* Register a handler for an interrupt number. The trap dispatcher calls
 * it with interrupts disabled and acknowledges the interrupt afterwards. */
void irq_register(unsigned irq, irq_handler_fn fn, void *arg);
/* Route a global system interrupt of the firmware, such as the SCI of ACPI
 * or a line of the device tree, to fn and enable it. flags are
 * IRQ_GSI_LEVEL or edge triggered, and IRQ_GSI_ACTIVE_LOW or active high.
 * Returns the interrupt number or a negative errno value. */
#define IRQ_GSI_LEVEL      1
#define IRQ_GSI_ACTIVE_LOW 2
int irq_route_gsi(unsigned gsi, unsigned flags, irq_handler_fn fn, void *arg);
void irq_dispatch(struct trapframe *tf);
/* Allocate an interrupt number for a message signalled device interrupt
 * (MSI-X). Returns the number or -ENOSPC. */
int irq_alloc(void);
/* Send interrupt irq to the CPU with kernel id cpu. Waits until the
 * previous IPI from this CPU has been delivered. */
void arch_send_ipi(unsigned cpu, unsigned irq);
