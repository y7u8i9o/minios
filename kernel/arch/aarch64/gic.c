#define KLOG_SUBSYS "gic"
#include <arch/irq.h>
#include <arch/init.h>
#include <arch/cpu.h>
#include <arch/trap.h>
#include <mm/vmm.h>
#include <klog.h>
#include <debug/panic.h>
#include "gic.h"
#include "timer_internal.h"
#include "devtree.h"
#include "its.h"

/* The GICv3 (A5): the distributor and the redistributors, 128 KiB per
 * CPU, at the addresses of the device tree (A7). The CPU interface is
 * reached through the ICC system registers. Every interrupt is in group 1.
 * Shared peripheral interrupts are routed to the CPU that registers them,
 * the boot CPU. SGIs are the IPIs (A8). LPIs, the interrupts of MSI, are
 * in its.c. */

#define GICR_STRIDE     0x20000UL

#define GICD_CTLR       0x0000
#define GICD_TYPER      0x0004
#define GICD_IGROUPR    0x0080
#define GICD_ISENABLER  0x0100
#define GICD_IPRIORITYR 0x0400
#define GICD_IROUTER    0x6000
#define CTLR_RWP        (1u << 31)
#define CTLR_ARE        (1u << 4)
#define CTLR_GRP1       (1u << 1)
#define CTLR_GRP0       (1u << 0)

#define GICR_WAKER      0x0014
#define GICR_TYPER      0x0008
#define WAKER_SLEEP     (1u << 1)
#define WAKER_ASLEEP    (1u << 2)
#define GICR_SGI        0x10000         /* SGI and PPI frame */
#define GICR_IGROUPR0   (GICR_SGI + 0x0080)
#define GICR_ISENABLER0 (GICR_SGI + 0x0100)
#define GICR_IPRIORITYR (GICR_SGI + 0x0400)

#define DEFAULT_PRIORITY 0xa0
#define MAX_IRQS        1020

static volatile uint8_t *gicd;
static volatile uint8_t *gicr_base;     /* every redistributor, mapped once */
static unsigned gicr_count;
static unsigned nlines;

/* Handler table. Written only during initialization, before the interrupt
 * is enabled, so the interrupt path reads it without a lock. */
static struct {
    irq_handler_fn fn;
    void *arg;
} handlers[MAX_IRQS];

static inline uint32_t rd32(volatile uint8_t *base, unsigned off)
{
    return *(volatile uint32_t *)(base + off);
}

static inline void wr32(volatile uint8_t *base, unsigned off, uint32_t v)
{
    *(volatile uint32_t *)(base + off) = v;
}

static void dist_wait(void)
{
    while (rd32(gicd, GICD_CTLR) & CTLR_RWP)
        cpu_relax();
}

/* The redistributor whose affinity matches the calling CPU, and its
 * physical address. */
static volatile uint8_t *find_redistributor(uintptr_t *phys)
{
    uint64_t mpidr;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    uint32_t aff = (uint32_t)((mpidr & 0xffffff) | ((mpidr >> 8) & 0xff000000));
    for (unsigned i = 0; i < gicr_count; i++) {
        volatile uint8_t *r = gicr_base + i * GICR_STRIDE;
        uint64_t typer = *(volatile uint64_t *)(r + GICR_TYPER);
        if ((uint32_t)(typer >> 32) == aff) {
            *phys = devtree.gicr + i * GICR_STRIDE;
            return r;
        }
        if (typer & (1u << 4))          /* Last */
            break;
    }
    panic("gic: no redistributor for mpidr %lx", mpidr);
}

/* Wake the redistributor of the calling CPU, put its SGIs and PPIs in
 * group 1 at the default priority, enable those that have a handler, and
 * enable the CPU interface. */
static uintptr_t init_cpu_interface(struct cpu *c)
{
    uintptr_t phys;
    volatile uint8_t *gicr = find_redistributor(&phys);
    c->arch.gicr = gicr;
    wr32(gicr, GICR_WAKER, rd32(gicr, GICR_WAKER) & ~WAKER_SLEEP);
    while (rd32(gicr, GICR_WAKER) & WAKER_ASLEEP)
        cpu_relax();
    /* Under HVF the GIC of Hypervisor.framework reports the SGIs and PPIs
     * in group 1 already and does not complete a write of IGROUPR0, so
     * these registers are written only when their value differs. */
    if (rd32(gicr, GICR_IGROUPR0) != 0xffffffff)
        wr32(gicr, GICR_IGROUPR0, 0xffffffff);
    for (unsigned i = 0; i < 32; i += 4) {
        if (rd32(gicr, GICR_IPRIORITYR + i) != 0x01010101u * DEFAULT_PRIORITY)
            wr32(gicr, GICR_IPRIORITYR + i, 0x01010101u * DEFAULT_PRIORITY);
    }
    uint32_t enabled = 0;
    for (unsigned irq = 0; irq < 32; irq++)
        if (handlers[irq].fn)
            enabled |= 1u << irq;
    if (enabled)
        wr32(gicr, GICR_ISENABLER0, enabled);

    /* The CPU interface: system register access, every priority, group 1. */
    uint64_t sre;
    __asm__ volatile("mrs %0, icc_sre_el1" : "=r"(sre));
    __asm__ volatile("msr icc_sre_el1, %0; isb" : : "r"(sre | 1) : "memory");
    __asm__ volatile("msr icc_pmr_el1, %0; msr icc_bpr1_el1, xzr; msr icc_igrpen1_el1, %1; isb"
                     : : "r"(0xffUL), "r"(1UL) : "memory");
    return phys;
}

void arch_init_interrupts(void)
{
    gicd = vmm_map_mmio(devtree.gicd, 0x10000, VM_KERNEL_RW | VM_NOCACHE);
    if (!gicd)
        panic("gic: cannot map the distributor");
    gicr_count = (unsigned)(devtree.gicr_size / GICR_STRIDE);
    gicr_base = vmm_map_mmio(devtree.gicr, gicr_count * GICR_STRIDE, VM_KERNEL_RW | VM_NOCACHE);
    if (!gicr_base)
        panic("gic: cannot map the redistributors");
    nlines = ((rd32(gicd, GICD_TYPER) & 0x1f) + 1) * 32;
    if (nlines > MAX_IRQS)
        nlines = MAX_IRQS;
    wr32(gicd, GICD_CTLR, 0);
    dist_wait();
    for (unsigned i = 32; i < nlines; i += 32)
        wr32(gicd, GICD_IGROUPR + i / 8, 0xffffffff);
    for (unsigned i = 32; i < nlines; i += 4)
        wr32(gicd, GICD_IPRIORITYR + i, 0x01010101u * DEFAULT_PRIORITY);
    wr32(gicd, GICD_CTLR, CTLR_ARE | CTLR_GRP1 | CTLR_GRP0);
    dist_wait();

    struct cpu *c = cpu_current();
    uintptr_t gicr_phys = init_cpu_interface(c);
    klog_info("gicv3: distributor at %lx with %u interrupt lines, redistributor at %lx",
              devtree.gicd, nlines, gicr_phys);
    its_init(c->arch.gicr, gicr_phys);
}

void gic_init_cpu(void)
{
    init_cpu_interface(cpu_current());
}

void irq_register(unsigned irq, irq_handler_fn fn, void *arg)
{
    if (irq >= LPI_BASE) {
        its_register(irq, fn, arg);
        return;
    }
    if (irq >= nlines)
        panic("irq_register: interrupt %u beyond the %u lines of the gic", irq, nlines);
    handlers[irq].fn = fn;
    handlers[irq].arg = arg;
    __asm__ volatile("dsb ish" : : : "memory");
    if (irq < 32) {
        /* An SGI or PPI is enabled in the redistributor of the calling
         * CPU. gic_init_cpu enables it on the application processors,
         * which start after every SGI and PPI is registered. */
        wr32(cpu_current()->arch.gicr, GICR_ISENABLER0, 1u << irq);
    } else {
        uint64_t mpidr;
        __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
        *(volatile uint64_t *)(gicd + GICD_IROUTER + 8 * irq) = mpidr & 0xff00ffffffUL;
        wr32(gicd, GICD_ISENABLER + (irq / 32) * 4, 1u << (irq % 32));
    }
}

void irq_dispatch(struct trapframe *tf)
{
    uint64_t iar;
    __asm__ volatile("mrs %0, icc_iar1_el1" : "=r"(iar));
    unsigned irq = iar & 0xffffff;
    if (irq >= 1020 && irq <= 1023)
        return;                         /* spurious: no end of interrupt */
    if (irq == IRQ_TIMER)
        timer_rearm();
    if (irq >= LPI_BASE)
        its_dispatch(tf, irq);
    else if (irq < MAX_IRQS && handlers[irq].fn)
        handlers[irq].fn(tf, handlers[irq].arg);
    else
        klog_warn("unhandled interrupt %u", irq);
    __asm__ volatile("msr icc_eoir1_el1, %0; isb" : : "r"(iar) : "memory");
}

/* Interrupt numbers for MSI are LPIs. */
int irq_alloc(void)
{
    return its_alloc();
}

/* An SGI to one CPU through ICC_SGI1R_EL1: the affinity levels 3 to 1
 * of its MPIDR and a bit for affinity level 0 in the target list, whose
 * range selector RS covers 16 values of affinity 0. */
void arch_send_ipi(unsigned cpu, unsigned irq)
{
    uint64_t mpidr = cpu_by_id(cpu)->arch.mpidr;
    uint64_t aff0 = mpidr & 0xff;
    uint64_t v = ((mpidr >> 32) & 0xff) << 48 | ((mpidr >> 16) & 0xff) << 32 |
                 (uint64_t)(irq & 0xf) << 24 | ((mpidr >> 8) & 0xff) << 16 |
                 (aff0 >> 4) << 44 | 1UL << (aff0 & 0xf);
    /* The stores the receiver reads are visible before the interrupt. */
    __asm__ volatile("dsb ish; msr icc_sgi1r_el1, %0; isb" : : "r"(v) : "memory");
}
