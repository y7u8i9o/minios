#define KLOG_SUBSYS "irq"
#include <arch/irq.h>
#include <arch/apic.h>
#include <arch/trap.h>
#include <klog.h>
#include <debug/panic.h>

/* Handler table. Written only during initialization, before the vector is
 * unmasked, so the interrupt path reads it without a lock. */
static struct {
    irq_handler_fn fn;
    void *arg;
} handlers[256];

void irq_register(uint8_t vector, irq_handler_fn fn, void *arg)
{
    if (vector < IRQ_VECTOR_BASE)
        panic("irq_register: vector %u is an exception", vector);
    handlers[vector].fn = fn;
    handlers[vector].arg = arg;
}

void irq_dispatch(struct trapframe *tf)
{
    uint8_t vec = (uint8_t)tf->vector;
    if (vec == IRQ_SPURIOUS)
        return;   /* no EOI for spurious interrupts */
    if (handlers[vec].fn)
        handlers[vec].fn(tf, handlers[vec].arg);
    else
        klog_warn("unhandled interrupt vector %u", vec);
    lapic_eoi();
}
