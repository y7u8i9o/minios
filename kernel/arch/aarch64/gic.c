#define KLOG_SUBSYS "gic"
#include <arch/irq.h>
#include <arch/init.h>
#include <arch/cpu.h>
#include <arch/trap.h>
#include <mm/vmm.h>
#include <klog.h>
#include <errno.h>
#include <debug/panic.h>
#include "gic.h"
#include "timer_internal.h"
#include "devtree.h"
#include <drivers/devinfo.h>
#include "its.h"

/* The GICv3 (A5) has a distributor and one redistributor of 128 KiB per
 * CPU at the addresses of the device tree (A7), and its CPU interface is
 * reached through the ICC system registers. Every interrupt is in group 1.
 * Shared peripheral interrupts are routed to the CPU that registers them,
 * the boot CPU. SGIs are the IPIs (A8). LPIs, the interrupts of MSI, are
 * in its.c.
 *
 * A GICv2, which the device tree describes by the compatible string of its
 * implementation, has a distributor whose SGI and PPI registers are banked
 * per CPU and a CPU interface in memory. Shared peripheral interrupts are
 * routed through the target bits of the boot CPU, and SGIs are sent
 * through GICD_SGIR. MSI uses the shared peripheral interrupts of a GICv2m
 * frame, which raise an interrupt when its number is written to
 * MSI_SETSPI_NS. A GICv2 serves at most 8 CPUs. */

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

#define GICD_ITARGETSR  0x0800          /* GICv2 */
#define GICD_ICFGR      0x0c00
#define GICD_SGIR       0x0f00          /* GICv2 */
#define ICFGR_EDGE      2u

#define GICC_CTLR       0x0000          /* GICv2 CPU interface */
#define GICC_PMR        0x0004
#define GICC_BPR        0x0008
#define GICC_IAR        0x000c
#define GICC_EOIR       0x0010

#define V2M_TYPER       0x0008          /* GICv2m frame */
#define V2M_SETSPI      0x0040

#define DEFAULT_PRIORITY 0xa0
#define MAX_IRQS        1020

static int v2;                          /* a GICv2, set once by arch_init_interrupts */
static volatile uint8_t *gicd;
static volatile uint8_t *gicc;          /* GICv2 */
static unsigned v2m_base, v2m_count;    /* the MSI interrupts of the GICv2m frame */
static unsigned v2m_next;               /* atomic, the next one irq_alloc hands out */
static volatile uint8_t *gicr_base;     /* every redistributor, mapped once */
static unsigned gicr_count;
static unsigned nlines;

/* Handler table. Written only during initialization, before the interrupt
 * is enabled, so the interrupt path reads it without a lock. */
static struct {
    irq_handler_fn fn;
    void *arg;
} handlers[MAX_IRQS];

/* Register accesses are single loads and stores without writeback. QEMU
 * emulates the registers of a GICv2 by decoding the trapped access, which
 * the syndrome of an access with writeback or of a load pair does not
 * describe, and its HVF backend stops on such an access. */
static inline uint32_t rd32(volatile uint8_t *base, unsigned off)
{
    uint32_t v;
    __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(base + off) : "memory");
    return v;
}

static inline void wr32(volatile uint8_t *base, unsigned off, uint32_t v)
{
    __asm__ volatile("str %w0, [%1]" : : "rZ"(v), "r"(base + off) : "memory");
}

static inline void wr8(volatile uint8_t *base, unsigned off, uint8_t v)
{
    __asm__ volatile("strb %w0, [%1]" : : "rZ"(v), "r"(base + off) : "memory");
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

/* The GICv2 part of a CPU. The distributor banks the registers of SGIs
 * and PPIs per CPU, and the CPU interface is in memory. */
static void init_cpu_interface_v2(struct cpu *c)
{
    /* The banked target register of SGI 0 contains the bit of the calling
     * CPU. An implementation for one CPU reads it as zero. */
    uint8_t mask = (uint8_t)(rd32(gicd, GICD_ITARGETSR) & 0xff);
    c->arch.gic_mask = mask ? mask : 1;
    for (unsigned i = 0; i < 32; i += 4)
        wr32(gicd, GICD_IPRIORITYR + i, 0x01010101u * DEFAULT_PRIORITY);
    uint32_t enabled = 0;
    for (unsigned irq = 0; irq < 32; irq++)
        if (handlers[irq].fn)
            enabled |= 1u << irq;
    if (enabled)
        wr32(gicd, GICD_ISENABLER, enabled);
    wr32(gicc, GICC_PMR, 0xff);
    wr32(gicc, GICC_BPR, 0);
    wr32(gicc, GICC_CTLR, 1);
}

static void init_v2(void)
{
    gicc = vmm_map_mmio(devtree.gicc, 0x2000, VM_KERNEL_RW | VM_NOCACHE);
    if (!gicc)
        panic("gic: cannot map the cpu interface");
    wr32(gicd, GICD_CTLR, 0);
    for (unsigned i = 32; i < nlines; i += 4)
        wr32(gicd, GICD_IPRIORITYR + i, 0x01010101u * DEFAULT_PRIORITY);
    if (devtree.v2m) {
        volatile uint8_t *v2m = vmm_map_mmio(devtree.v2m, 0x1000, VM_KERNEL_RW | VM_NOCACHE);
        if (!v2m)
            panic("gic: cannot map the v2m frame");
        uint32_t typer = rd32(v2m, V2M_TYPER);
        v2m_base = (typer >> 16) & 0x3ff;
        v2m_count = typer & 0x3ff;
        if (v2m_base + v2m_count > nlines)
            v2m_count = v2m_base < nlines ? nlines - v2m_base : 0;
        /* MSIs are edge triggered. */
        for (unsigned irq = v2m_base; irq < v2m_base + v2m_count; irq++) {
            unsigned off = GICD_ICFGR + (irq / 16) * 4;
            wr32(gicd, off, rd32(gicd, off) | ICFGR_EDGE << (irq % 16) * 2);
        }
    }
    wr32(gicd, GICD_CTLR, 1);
    init_cpu_interface_v2(cpu_current());
    klog_info("gicv2: distributor at %lx with %u interrupt lines, cpu interface at %lx, %u msi interrupts from %u",
              devtree.gicd, nlines, devtree.gicc, v2m_count, v2m_base);
}

void arch_init_interrupts(void)
{
    gicd = vmm_map_mmio(devtree.gicd, 0x10000, VM_KERNEL_RW | VM_NOCACHE);
    if (!gicd)
        panic("gic: cannot map the distributor");
    if (devtree.gic_version == 2) {
        v2 = 1;
        nlines = ((rd32(gicd, GICD_TYPER) & 0x1f) + 1) * 32;
        if (nlines > MAX_IRQS)
            nlines = MAX_IRQS;
        init_v2();
        return;
    }
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
    struct cpu *c = cpu_current();
    if (v2) {
        init_cpu_interface_v2(c);
        return;
    }
    uintptr_t gicr_phys = init_cpu_interface(c);
    its_init_cpu(c->arch.gicr, gicr_phys);
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
    if (v2) {
        /* SGIs and PPIs are enabled in the banked register of the calling
         * CPU, and a shared interrupt is routed to that CPU. */
        if (irq >= 32)
            wr8(gicd, GICD_ITARGETSR + irq, cpu_current()->arch.gic_mask);
        wr32(gicd, GICD_ISENABLER + (irq / 32) * 4, 1u << (irq % 32));
    } else if (irq < 32) {
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

/* A global system interrupt of ACPI or of the device tree is the number of
 * a shared peripheral interrupt. The GIC has no active low inputs. The
 * trigger mode is set in GICD_ICFGR while the interrupt is still
 * disabled. */
int irq_route_gsi(unsigned gsi, unsigned flags, irq_handler_fn fn, void *arg)
{
    if (gsi < 32 || gsi >= nlines)
        return -EINVAL;
    if (flags & IRQ_GSI_ACTIVE_LOW)
        return -EOPNOTSUPP;
    unsigned off = GICD_ICFGR + (gsi / 16) * 4, shift = (gsi % 16) * 2;
    uint32_t cfg = rd32(gicd, off) & ~(ICFGR_EDGE << shift);
    wr32(gicd, off, cfg | ((flags & IRQ_GSI_LEVEL) ? 0 : ICFGR_EDGE << shift));
    irq_register(gsi, fn, arg);
    return (int)gsi;
}

static void dispatch_v2(struct trapframe *tf)
{
    /* The IAR of an SGI also names the sending CPU, and the end of the
     * interrupt is written with the whole value. */
    uint32_t iar = rd32(gicc, GICC_IAR);
    unsigned irq = iar & 0x3ff;
    if (irq >= 1020)
        return;                         /* spurious: no end of interrupt */
    if (irq == IRQ_TIMER)
        timer_rearm();
    if (handlers[irq].fn)
        handlers[irq].fn(tf, handlers[irq].arg);
    else
        klog_warn("unhandled interrupt %u", irq);
    wr32(gicc, GICC_EOIR, iar);
}

void irq_dispatch(struct trapframe *tf)
{
    if (v2) {
        dispatch_v2(tf);
        return;
    }
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

/* Interrupt numbers for MSI are LPIs on a GICv3 and the shared interrupts
 * of the v2m frame on a GICv2. */
int irq_alloc(void)
{
    if (!v2)
        return its_alloc();
    if (!v2m_count)
        return -ENODEV;
    unsigned n = __atomic_fetch_add(&v2m_next, 1, __ATOMIC_RELAXED);
    if (n >= v2m_count)
        return -ENOSPC;
    return (int)(v2m_base + n);
}

void gic_v2m_msi_compose(unsigned irq, uint64_t *addr, uint32_t *data)
{
    *addr = devtree.v2m + V2M_SETSPI;
    *data = irq;
}

/* An SGI to one CPU through ICC_SGI1R_EL1: the affinity levels 3 to 1
 * of its MPIDR and a bit for affinity level 0 in the target list, whose
 * range selector RS covers 16 values of affinity 0. */
void arch_send_ipi(unsigned cpu, unsigned irq)
{
    if (v2) {
        /* GICD_SGIR names the targets by their CPU interface bits. */
        uint32_t target = cpu_by_id(cpu)->arch.gic_mask;
        __asm__ volatile("dsb ish" : : : "memory");
        wr32(gicd, GICD_SGIR, target << 16 | (irq & 0xf));
        return;
    }
    uint64_t mpidr = cpu_by_id(cpu)->arch.mpidr;
    uint64_t aff0 = mpidr & 0xff;
    uint64_t v = ((mpidr >> 32) & 0xff) << 48 | ((mpidr >> 16) & 0xff) << 32 |
                 (uint64_t)(irq & 0xf) << 24 | ((mpidr >> 8) & 0xff) << 16 |
                 (aff0 >> 4) << 44 | 1UL << (aff0 & 0xf);
    /* The stores the receiver reads are visible before the interrupt. */
    __asm__ volatile("dsb ish; msr icc_sgi1r_el1, %0; isb" : : "r"(v) : "memory");
}

/* The GIC for /dev/devices. The fields are written once by
 * arch_init_interrupts. */
void gic_describe(struct devinfo *d)
{
    devinfo_prop(d, "interrupt_controller", "GICv%u", v2 ? 2 : 3);
    devinfo_prop(d, "gic_distributor", "0x%lx, %u interrupt lines", (unsigned long)devtree.gicd, nlines);
    if (v2) {
        devinfo_prop(d, "gic_cpu_interface", "0x%lx", (unsigned long)devtree.gicc);
        if (v2m_count)
            devinfo_prop(d, "gicv2m", "0x%lx, interrupts %u to %u, %u allocated", (unsigned long)devtree.v2m,
                         v2m_base, v2m_base + v2m_count - 1,
                         MIN(__atomic_load_n(&v2m_next, __ATOMIC_RELAXED), v2m_count));
    } else {
        devinfo_prop(d, "gic_redistributors", "0x%lx, %u frames", (unsigned long)devtree.gicr, gicr_count);
        its_describe(d);
    }
}
