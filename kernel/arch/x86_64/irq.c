#define KLOG_SUBSYS "irq"
#include <arch/irq.h>
#include <arch/apic.h>
#include <arch/trap.h>
#include <arch/cpu.h>
#include <klog.h>
#include <debug/panic.h>
#include <errno.h>

/* Handler table. Written only during initialization, before the vector is
 * unmasked, so the interrupt path reads it without a lock. */
static struct {
    irq_handler_fn fn;
    void *arg;
} handlers[256];

/* Next vector for irq_alloc, advanced atomically. */
static unsigned next_dynamic = IRQ_DYNAMIC_BASE;

void irq_register(unsigned vector, irq_handler_fn fn, void *arg)
{
    if (vector < IRQ_VECTOR_BASE || vector > 0xff)
        panic("irq_register: vector %u is not an interrupt vector", vector);
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

int irq_alloc(void)
{
    unsigned v = __atomic_fetch_add(&next_dynamic, 1, __ATOMIC_RELAXED);
    if (v >= IRQ_TLB_SHOOTDOWN)
        return -ENOSPC;
    return (int)v;
}

void arch_send_ipi(unsigned cpu, unsigned irq)
{
    lapic_send_ipi(cpu_by_id(cpu)->arch.lapic_id, (uint8_t)irq);
}
