#define KLOG_SUBSYS "its"
#include "its.h"
#include "devtree.h"
#include <drivers/devinfo.h>
#include <arch/cpu.h>
#include <mm/memlayout.h>
#include <mm/pmm.h>
#include <mm/vmm.h>
#include <sync/spinlock.h>
#include <lib/string.h>
#include <klog.h>
#include <debug/panic.h>
#include <kassert.h>
#include <errno.h>

/* Redistributor LPI registers. */
#define GICR_CTLR       0x0000
#define GICR_TYPER      0x0008
#define GICR_PROPBASER  0x0070
#define GICR_PENDBASER  0x0078
#define GICR_CTLR_LPIS  (1u << 0)
#define GICR_TYPER_PLPIS (1u << 0)
#define GICD_TYPER      0x0004

/* ITS registers. */
#define GITS_CTLR       0x0000
#define GITS_TYPER      0x0008
#define GITS_CBASER     0x0080
#define GITS_CWRITER    0x0088
#define GITS_CREADR     0x0090
#define GITS_BASER(n)   (0x0100 + 8 * (n))
#define GITS_TRANSLATER 0x10040
#define GITS_CTLR_ENABLED (1u << 0)
#define GITS_CTLR_QUIESCENT (1u << 31)
#define GITS_TYPER_PTA  (1UL << 19)

#define BASER_VALID     (1UL << 63)
#define BASER_INDIRECT  (1UL << 62)
#define BASER_TYPE(v)   (((v) >> 56) & 7)
#define BASER_ENTRY(v)  ((((v) >> 48) & 0x1f) + 1)
#define BASER_TYPE_DEVICE 1
#define BASER_TYPE_COLLECTION 4
/* Inner write-back cacheable, inner shareable: the attributes of the
 * kernel's own mapping of the tables. The caches are also cleaned after
 * every write, for an implementation that ignores them. */
#define ATTR_WB_INNER   (7UL << 59 | 1UL << 10)
#define REG_ATTR_WB_INNER (7UL << 7 | 1UL << 10)

#define CMD_SYNC        0x05
#define CMD_MAPD        0x08
#define CMD_MAPC        0x09
#define CMD_MAPTI       0x0a
#define CMD_INV         0x0c
#define CMD_INVALL      0x0d

#define LPI_ID_BITS     14              /* LPIs 8192 to 16383 */
#define LPI_PRIORITY    0xa0
#define LPI_PROP_RES1   (1u << 1)
#define LPI_PROP_ENABLE (1u << 0)
#define EVENT_BITS      5               /* events per device */
#define CMDQ_SIZE       PAGE_SIZE
#define MAX_DEVICES     32

/* Handler table, written by its_register before the device can raise the
 * LPI, read by the interrupt path without a lock (as in gic.c). */
static struct {
    irq_handler_fn fn;
    void *arg;
} handlers[LPI_MAX];
static unsigned next_lpi;               /* advanced atomically by its_alloc */

static uint8_t *prop_table;             /* LPI configuration, one byte per LPI */

/* The ITS state below is protected by its_lock: the command queue, the
 * device table and its level 2 pages, and the mapped devices. */
static DEFINE_SPINLOCK(its_lock);
static volatile uint8_t *its;
static uintptr_t its_phys;
static uint64_t *cmdq;
static uintptr_t cmdq_phys;
static unsigned cmdq_write;
static bool its_pta;                    /* GITS_TYPER.PTA: collections name redistributor addresses */
static uintptr_t prop_phys;             /* the configuration table, shared by every redistributor */
static unsigned itt_entry_size;
static uint64_t *device_l1;             /* level 1 device table, or the flat table */
static bool device_indirect;
static unsigned device_entry_size, device_bits;
static struct its_device {
    uint32_t id;
    unsigned next_event;
} devices[MAX_DEVICES];
static unsigned ndevices;
/* Device ID, event and target CPU of each LPI mapped through the ITS. */
static struct {
    uint32_t devid, event;
    unsigned cpu;
    bool mapped;
} lpi_map[LPI_MAX];

static inline uint64_t rd64(volatile uint8_t *base, unsigned off)
{
    return *(volatile uint64_t *)(base + off);
}

static inline void wr64(volatile uint8_t *base, unsigned off, uint64_t v)
{
    *(volatile uint64_t *)(base + off) = v;
}

static inline uint32_t rd32(volatile uint8_t *base, unsigned off)
{
    return *(volatile uint32_t *)(base + off);
}

static inline void wr32(volatile uint8_t *base, unsigned off, uint32_t v)
{
    *(volatile uint32_t *)(base + off) = v;
}

/* Clean and invalidate the data cache lines of [p, p + size) to the point
 * of coherency, for memory the GIC reads. */
static void clean_range(const void *p, size_t size)
{
    uintptr_t a = (uintptr_t)p & ~63UL;
    for (; a < (uintptr_t)p + size; a += 64)
        __asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
    __asm__ volatile("dsb sy" : : : "memory");
}

/* Zeroed, physically contiguous memory of 2^order pages. */
static void *alloc_zeroed(unsigned order, uintptr_t *phys)
{
    struct page *pg = pmm_alloc(order);
    if (!pg)
        panic("its: out of memory for the interrupt tables");
    *phys = page_to_phys(pg);
    void *va = P2V(*phys);
    memset(va, 0, PAGE_SIZE << order);
    clean_range(va, PAGE_SIZE << order);
    return va;
}

/* ---- the command queue, under its_lock ---- */

static void its_command(uint64_t d0, uint64_t d1, uint64_t d2)
{
    kassert(spin_locked_by_current(&its_lock));
    unsigned next = (cmdq_write + 32) % CMDQ_SIZE;
    while ((rd64(its, GITS_CREADR) & 0xfffe0) == next)
        cpu_relax();                    /* the queue is full */
    uint64_t *c = cmdq + cmdq_write / 8;
    c[0] = d0;
    c[1] = d1;
    c[2] = d2;
    c[3] = 0;
    clean_range(c, 32);
    cmdq_write = next;
    wr64(its, GITS_CWRITER, cmdq_write);
}

/* Wait until the ITS has executed every queued command, synchronizing
 * with the redistributor that rdbase names. */
static void its_wait(uint64_t rdbase)
{
    kassert(spin_locked_by_current(&its_lock));
    its_command(CMD_SYNC, 0, rdbase);
    while ((rd64(its, GITS_CREADR) & 0xfffe0) != cmdq_write)
        cpu_relax();
}

/* ---- tables ---- */

/* Program one GITS_BASER register: the device table as an indirect table
 * when the ITS supports it, the collection table flat in one page. */
static void setup_baser(unsigned n)
{
    uint64_t v = rd64(its, GITS_BASER(n));
    unsigned type = BASER_TYPE(v);
    if (type != BASER_TYPE_DEVICE && type != BASER_TYPE_COLLECTION)
        return;
    unsigned entry = BASER_ENTRY(v);
    uintptr_t pa;
    if (type == BASER_TYPE_COLLECTION) {
        alloc_zeroed(0, &pa);
        wr64(its, GITS_BASER(n), BASER_VALID | ATTR_WB_INNER | ((uint64_t)entry - 1) << 48 | pa);
        return;
    }
    device_entry_size = entry;
    /* Indirect: one page of level 1 entries of 8 bytes, each pointing to a
     * page of device entries. */
    device_l1 = alloc_zeroed(0, &pa);
    wr64(its, GITS_BASER(n), BASER_VALID | BASER_INDIRECT | ATTR_WB_INNER |
                             ((uint64_t)entry - 1) << 48 | pa);
    if (rd64(its, GITS_BASER(n)) & BASER_INDIRECT) {
        device_indirect = true;
        unsigned per_page = PAGE_SIZE / entry;
        unsigned bits = 0;
        while ((1u << bits) < (PAGE_SIZE / 8) * per_page)
            bits++;
        if (bits < device_bits)
            device_bits = bits;
        return;
    }
    /* Flat: room for the device IDs of 16 buses. */
    pmm_free(phys_to_page(pa), 0);
    unsigned ids = 1u << 12;
    if (device_bits > 12)
        device_bits = 12;
    unsigned pages = (unsigned)ALIGN_UP((size_t)ids * entry, PAGE_SIZE) / PAGE_SIZE;
    unsigned order = 0;
    while ((1u << order) < pages)
        order++;
    device_l1 = alloc_zeroed(order, &pa);
    wr64(its, GITS_BASER(n), BASER_VALID | ATTR_WB_INNER | ((uint64_t)entry - 1) << 48 |
                             pa | ((1u << order) - 1));
}

/* Make the device table cover devid: allocate its level 2 page. */
static bool device_table_cover(uint32_t devid)
{
    if (devid >> device_bits)
        return false;
    if (!device_indirect)
        return true;
    unsigned per_page = PAGE_SIZE / device_entry_size;
    uint64_t *l1 = &device_l1[devid / per_page];
    if (!(*l1 & BASER_VALID)) {
        uintptr_t pa;
        alloc_zeroed(0, &pa);
        *l1 = BASER_VALID | pa;
        clean_range(l1, 8);
    }
    return true;
}

static struct its_device *device_get(uint32_t devid)
{
    for (unsigned i = 0; i < ndevices; i++)
        if (devices[i].id == devid)
            return &devices[i];
    if (ndevices == MAX_DEVICES || !device_table_cover(devid))
        return NULL;
    uintptr_t itt;
    size_t itt_size = ((size_t)itt_entry_size << EVENT_BITS);
    kassert(itt_size <= PAGE_SIZE);
    alloc_zeroed(0, &itt);
    its_command(CMD_MAPD | (uint64_t)devid << 32, EVENT_BITS - 1, BASER_VALID | itt);
    struct its_device *d = &devices[ndevices++];
    d->id = devid;
    d->next_event = 0;
    return d;
}

/* ---- the redistributor ---- */

/* The LPI tables of one redistributor: the configuration table, one byte
 * per LPI from 8192, which every redistributor shares and the boot CPU
 * allocates, and a pending table of its own. */
static void setup_lpis(volatile uint8_t *gicr, uintptr_t gicr_phys)
{
    if (!(rd64(gicr, GICR_TYPER) & GICR_TYPER_PLPIS))
        panic("gic: the redistributor at %lx has no LPIs", gicr_phys);
    if (rd32(gicr, GICR_CTLR) & GICR_CTLR_LPIS)
        panic("gic: LPIs were enabled before the kernel");
    if (!prop_table) {
        size_t prop_size = (1UL << LPI_ID_BITS) - LPI_BASE;
        prop_table = alloc_zeroed(1, &prop_phys);
        kassert(prop_size <= 2 * PAGE_SIZE);
        memset(prop_table, LPI_PRIORITY | LPI_PROP_RES1, prop_size);
        clean_range(prop_table, prop_size);
    }
    /* Pending bits for every interrupt ID, 64 KiB aligned. */
    uintptr_t pend_phys;
    alloc_zeroed(4, &pend_phys);
    wr64(gicr, GICR_PROPBASER, prop_phys | REG_ATTR_WB_INNER | (LPI_ID_BITS - 1));
    wr64(gicr, GICR_PENDBASER, pend_phys | REG_ATTR_WB_INNER | (1UL << 62));
    __asm__ volatile("dsb sy" : : : "memory");
    wr32(gicr, GICR_CTLR, rd32(gicr, GICR_CTLR) | GICR_CTLR_LPIS);
    __asm__ volatile("dsb sy; isb" : : : "memory");
}

/* Map the collection of the calling CPU, numbered by its id, to its
 * redistributor. The ITS names the redistributor by its physical address
 * (PTA) or by its processor number. */
static void map_collection(volatile uint8_t *gicr, uintptr_t gicr_phys)
{
    struct cpu *c = cpu_current();
    c->arch.its_rdbase = its_pta ? gicr_phys : ((rd64(gicr, GICR_TYPER) >> 8) & 0xffff) << 16;
    spin_lock(&its_lock);
    its_command(CMD_MAPC, 0, BASER_VALID | c->arch.its_rdbase | c->id);
    its_command(CMD_INVALL, 0, c->id);
    its_wait(c->arch.its_rdbase);
    spin_unlock(&its_lock);
}

void its_init_cpu(volatile uint8_t *gicr, uintptr_t gicr_phys)
{
    if (!its)
        return;
    setup_lpis(gicr, gicr_phys);
    map_collection(gicr, gicr_phys);
}

void its_init(volatile uint8_t *gicr, uintptr_t gicr_phys)
{
    if (!devtree.its) {
        klog_warn("no its in the device tree, PCI devices have no interrupts");
        return;
    }
    setup_lpis(gicr, gicr_phys);
    its_phys = devtree.its;
    its = vmm_map_mmio(its_phys, 0x20000, VM_KERNEL_RW | VM_NOCACHE);
    if (!its)
        panic("its: cannot map %lx", its_phys);
    if (rd32(its, GITS_CTLR) & GITS_CTLR_ENABLED) {
        wr32(its, GITS_CTLR, 0);
        while (!(rd32(its, GITS_CTLR) & GITS_CTLR_QUIESCENT))
            cpu_relax();
    }
    uint64_t typer = rd64(its, GITS_TYPER);
    itt_entry_size = (unsigned)((typer >> 4) & 0xf) + 1;
    device_bits = (unsigned)((typer >> 13) & 0x1f) + 1;
    its_pta = (typer & GITS_TYPER_PTA) != 0;

    cmdq = alloc_zeroed(0, &cmdq_phys);
    wr64(its, GITS_CBASER, BASER_VALID | ATTR_WB_INNER | cmdq_phys | (CMDQ_SIZE / PAGE_SIZE - 1));
    wr64(its, GITS_CWRITER, 0);
    for (unsigned n = 0; n < 8; n++)
        setup_baser(n);
    if (!device_l1)
        panic("its: no device table");
    wr32(its, GITS_CTLR, GITS_CTLR_ENABLED);

    map_collection(gicr, gicr_phys);
    klog_info("its at %lx, %u bit device ids in a%s table, lpis %u to %u", its_phys,
              device_bits, device_indirect ? "n indirect" : " flat", LPI_BASE,
              LPI_BASE + LPI_MAX - 1);
}

int its_alloc(void)
{
    if (!its)
        return -ENODEV;
    unsigned n = __atomic_fetch_add(&next_lpi, 1, __ATOMIC_RELAXED);
    if (n >= LPI_MAX)
        return -ENOSPC;
    return (int)(LPI_BASE + n);
}

void its_register(unsigned lpi, irq_handler_fn fn, void *arg)
{
    unsigned n = lpi - LPI_BASE;
    if (lpi < LPI_BASE || n >= LPI_MAX || !its)
        panic("its_register: %u is not an allocated lpi", lpi);
    handlers[n].fn = fn;
    handlers[n].arg = arg;
    prop_table[n] = LPI_PRIORITY | LPI_PROP_RES1 | LPI_PROP_ENABLE;
    clean_range(&prop_table[n], 1);
    spin_lock(&its_lock);
    if (lpi_map[n].mapped) {
        its_command(CMD_INV | (uint64_t)lpi_map[n].devid << 32, lpi_map[n].event, 0);
        its_wait(cpu_by_id(lpi_map[n].cpu)->arch.its_rdbase);
    }
    spin_unlock(&its_lock);
}

void its_dispatch(struct trapframe *tf, unsigned lpi)
{
    unsigned n = lpi - LPI_BASE;
    if (n < LPI_MAX && handlers[n].fn)
        handlers[n].fn(tf, handlers[n].arg);
    else
        klog_warn("unhandled lpi %u", lpi);
}

void its_msi_compose(uint32_t devid, unsigned irq, unsigned cpu, uint64_t *addr, uint32_t *data)
{
    unsigned n = irq - LPI_BASE;
    if (irq < LPI_BASE || n >= LPI_MAX || !its)
        panic("its_msi_compose: %u is not an allocated lpi", irq);
    spin_lock(&its_lock);
    if (!lpi_map[n].mapped || lpi_map[n].devid != devid) {
        struct its_device *d = device_get(devid);
        if (!d || d->next_event >= (1u << EVENT_BITS))
            panic("its: no event for device %x", devid);
        /* The event is delivered to cpu through its collection. */
        struct cpu *c = cpu_by_id(cpu);
        lpi_map[n].devid = devid;
        lpi_map[n].event = d->next_event++;
        lpi_map[n].cpu = c->id;
        lpi_map[n].mapped = true;
        its_command(CMD_MAPTI | (uint64_t)devid << 32, lpi_map[n].event | (uint64_t)irq << 32, c->id);
        its_command(CMD_INV | (uint64_t)devid << 32, lpi_map[n].event, 0);
        its_wait(c->arch.its_rdbase);
        klog_info("lpi %u: device %x event %u on cpu %u", irq, devid, lpi_map[n].event, c->id);
    }
    *addr = its_phys + GITS_TRANSLATER;
    *data = lpi_map[n].event;
    spin_unlock(&its_lock);
}

/* The ITS for /dev/devices. The fields are written once by its_init. */
void its_describe(struct devinfo *d)
{
    if (!its)
        return;
    unsigned used = MIN(__atomic_load_n(&next_lpi, __ATOMIC_RELAXED), (unsigned)LPI_MAX);
    devinfo_prop(d, "its", "0x%lx, %u bit device IDs, %s device table", (unsigned long)its_phys, device_bits,
                 device_indirect ? "indirect" : "flat");
    devinfo_prop(d, "lpis", "%u to %u, %u allocated", LPI_BASE, LPI_BASE + LPI_MAX - 1, used);
}
