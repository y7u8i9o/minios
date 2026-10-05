#define KLOG_SUBSYS "apic"
#include <arch/apic.h>
#include <arch/cpu.h>
#include <arch/io.h>
#include <arch/pit.h>
#include <mm/vmm.h>
#include <klog.h>
#include <drivers/devinfo.h>
#include <debug/panic.h>

#define MSR_APIC_BASE       0x1b
#define APIC_BASE_ENABLE    (1UL << 11)

#define LAPIC_ID            0x020
#define LAPIC_VERSION       0x030
#define LAPIC_TPR           0x080
#define LAPIC_EOI           0x0b0
#define LAPIC_SVR           0x0f0
#define LAPIC_ESR           0x280
#define LAPIC_ICR_LO        0x300
#define LAPIC_ICR_HI        0x310
#define LAPIC_LVT_TIMER     0x320
#define LAPIC_LVT_LINT0     0x350
#define LAPIC_LVT_LINT1     0x360
#define LAPIC_LVT_ERROR     0x370
#define LAPIC_TIMER_INIT    0x380
#define LAPIC_TIMER_CUR     0x390
#define LAPIC_TIMER_DIV     0x3e0

#define SVR_ENABLE          (1u << 8)
#define LVT_MASKED          (1u << 16)
#define LVT_TIMER_PERIODIC  (1u << 17)
#define TIMER_DIV_16        0x3
#define ICR_DELIVERY_PENDING (1u << 12)

#define IOAPIC_DEFAULT_BASE 0xfec00000
#define IOAPIC_REGSEL       0x00
#define IOAPIC_WIN          0x10
#define IOAPIC_REG_VER      0x01
#define IOAPIC_REG_REDIR(n) (0x10 + 2 * (n))
#define IOAPIC_MASKED       (1u << 16)

static volatile uint32_t *lapic;
static volatile uint32_t *ioapic;
static unsigned ioapic_entries;
/* Timer ticks per second, measured once on the boot CPU. */
static uint64_t timer_ticks_per_second;

static inline uint32_t lapic_read(unsigned reg)
{
    return lapic[reg / 4];
}

static inline void lapic_write(unsigned reg, uint32_t v)
{
    lapic[reg / 4] = v;
    (void)lapic[LAPIC_ID / 4];   /* serialize */
}

void lapic_init(void)
{
    uint64_t base = rdmsr(MSR_APIC_BASE);
    uintptr_t pa = base & 0xfffff000UL;
    wrmsr(MSR_APIC_BASE, base | APIC_BASE_ENABLE);
    if (!lapic) {
        lapic = vmm_map_mmio(pa, PAGE_SIZE, VM_READ | VM_WRITE | VM_NOCACHE);
        if (!lapic)
            panic("apic: cannot map local APIC");
    }

    /* Mask the legacy 8259 PICs completely, they were remapped in idt_init. */
    outb(0x21, 0xff);
    outb(0xa1, 0xff);

    lapic_write(LAPIC_TPR, 0);
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT0, LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT1, LVT_MASKED);
    lapic_write(LAPIC_LVT_ERROR, LVT_MASKED);
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_ESR, 0);
    lapic_write(LAPIC_SVR, SVR_ENABLE | IRQ_SPURIOUS);
    lapic_write(LAPIC_EOI, 0);
    if (cpu_current()->id == 0) {
        uint32_t ver = lapic_read(LAPIC_VERSION);
        klog_info("local apic at %lx, id %u, version %02x, %u lvt entries, spurious vector %u",
                  pa, lapic_id(), ver & 0xff, ((ver >> 16) & 0xff) + 1, IRQ_SPURIOUS);
    }
}

uint32_t lapic_id(void)
{
    return lapic_read(LAPIC_ID) >> 24;
}

void lapic_eoi(void)
{
    lapic_write(LAPIC_EOI, 0);
}

uint64_t lapic_timer_calibrate(void)
{
    /* Count down from the maximum for 10 ms measured by the PIT. */
    lapic_write(LAPIC_TIMER_DIV, TIMER_DIV_16);
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED);
    lapic_write(LAPIC_TIMER_INIT, 0xffffffffu);
    pit_wait_us(10000);
    uint32_t elapsed = 0xffffffffu - lapic_read(LAPIC_TIMER_CUR);
    lapic_write(LAPIC_TIMER_INIT, 0);
    timer_ticks_per_second = (uint64_t)elapsed * 100;
    return timer_ticks_per_second;
}

void lapic_timer_start(unsigned hz)
{
    if (!timer_ticks_per_second)
        panic("apic: timer started before calibration");
    uint32_t count = (uint32_t)(timer_ticks_per_second / hz);
    if (count == 0)
        count = 1;
    lapic_write(LAPIC_TIMER_DIV, TIMER_DIV_16);
    lapic_write(LAPIC_LVT_TIMER, IRQ_TIMER | LVT_TIMER_PERIODIC);
    lapic_write(LAPIC_TIMER_INIT, count);
}

void lapic_send_ipi(uint32_t dest, uint8_t vector)
{
    while (lapic_read(LAPIC_ICR_LO) & ICR_DELIVERY_PENDING)
        cpu_relax();
    lapic_write(LAPIC_ICR_HI, dest << 24);
    /* Fixed delivery, physical destination, edge, no shorthand. */
    lapic_write(LAPIC_ICR_LO, vector);
}

static uint32_t ioapic_read(unsigned reg)
{
    ioapic[IOAPIC_REGSEL / 4] = reg;
    return ioapic[IOAPIC_WIN / 4];
}

static void ioapic_write(unsigned reg, uint32_t v)
{
    ioapic[IOAPIC_REGSEL / 4] = reg;
    ioapic[IOAPIC_WIN / 4] = v;
}

void ioapic_init(void)
{
    ioapic = vmm_map_mmio(IOAPIC_DEFAULT_BASE, PAGE_SIZE, VM_READ | VM_WRITE | VM_NOCACHE);
    if (!ioapic)
        panic("apic: cannot map I/O APIC");
    ioapic_entries = ((ioapic_read(IOAPIC_REG_VER) >> 16) & 0xff) + 1;
    for (unsigned i = 0; i < ioapic_entries; i++) {
        ioapic_write(IOAPIC_REG_REDIR(i), IOAPIC_MASKED | (IRQ_VECTOR_BASE + i));
        ioapic_write(IOAPIC_REG_REDIR(i) + 1, 0);
    }
    klog_info("i/o apic at %x, id %u, version %02x, %u redirection entries masked at vectors %u..%u",
              IOAPIC_DEFAULT_BASE, (ioapic_read(0) >> 24) & 0xf, ioapic_read(IOAPIC_REG_VER) & 0xff,
              ioapic_entries, IRQ_VECTOR_BASE, IRQ_VECTOR_BASE + ioapic_entries - 1);
}

void ioapic_route(unsigned gsi, uint8_t vector, bool masked)
{
    if (gsi >= ioapic_entries)
        panic("apic: gsi %u out of range", gsi);
    /* Fixed delivery, physical destination, active high, edge triggered,
     * routed to the boot CPU. */
    ioapic_write(IOAPIC_REG_REDIR(gsi) + 1, lapic_id() << 24);
    ioapic_write(IOAPIC_REG_REDIR(gsi), vector | (masked ? IOAPIC_MASKED : 0));
}

/* As ioapic_route, with the trigger mode and the polarity of the firmware:
 * bit 15 of the redirection entry selects level triggering and bit 13 an
 * active low input. Returns false for a GSI beyond the I/O APIC. */
bool ioapic_route_mode(unsigned gsi, uint8_t vector, bool level, bool active_low)
{
    if (gsi >= ioapic_entries)
        return false;
    ioapic_write(IOAPIC_REG_REDIR(gsi) + 1, lapic_id() << 24);
    ioapic_write(IOAPIC_REG_REDIR(gsi), vector | (level ? 1u << 15 : 0) | (active_low ? 1u << 13 : 0));
    return true;
}

void ioapic_mask(unsigned gsi, bool masked)
{
    uint32_t lo = ioapic_read(IOAPIC_REG_REDIR(gsi));
    lo = masked ? (lo | IOAPIC_MASKED) : (lo & ~IOAPIC_MASKED);
    ioapic_write(IOAPIC_REG_REDIR(gsi), lo);
}

/* The interrupt controllers for /dev/devices (docs/design/sysinfo.md). The
 * registers are read without a lock: the version registers do not change,
 * and an I/O APIC register read is a select and a read that the boot code
 * no longer performs. */
void apic_describe(struct devinfo *d)
{
    uint64_t base = rdmsr(MSR_APIC_BASE);
    devinfo_prop(d, "local_apic", "0x%lx", (unsigned long)(base & ~0xfffUL));
    devinfo_prop(d, "local_apic_mode", "%s", (base & (1UL << 10)) ? "x2apic" : (base & (1UL << 11)) ? "xapic" : "off");
    if (lapic) {
        uint32_t ver = lapic_read(LAPIC_VERSION);
        devinfo_prop(d, "local_apic_version", "0x%02x, %u LVT entries", ver & 0xff, ((ver >> 16) & 0xff) + 1);
    }
    devinfo_prop(d, "local_apic_timer", "%lu Hz, periodic", (unsigned long)timer_ticks_per_second);
    if (ioapic) {
        uint32_t ver = ioapic_read(IOAPIC_REG_VER);
        devinfo_prop(d, "io_apic", "0x%x, version 0x%02x, %u inputs", IOAPIC_DEFAULT_BASE, ver & 0xff, ioapic_entries);
    }
}
