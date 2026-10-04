#define KLOG_SUBSYS "pci"
#include <drivers/pci.h>
#include <arch/platform.h>
#include <arch/smp.h>
#include <arch/cpu.h>
#include <mm/vmm.h>
#include <klog.h>
#include <lib/printf.h>
#include <errno.h>
#include <drivers/devinfo.h>

static struct pci_dev devices[PCI_MAX_DEVICES];
static size_t ndevices;

static uint32_t raw_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    return platform_pci_read32(bus, slot, func, off);
}

uint32_t pci_read32(const struct pci_dev *d, uint8_t off)
{
    return raw_read32(d->bus, d->slot, d->func, off);
}

uint16_t pci_read16(const struct pci_dev *d, uint8_t off)
{
    return (uint16_t)(pci_read32(d, off) >> ((off & 2) * 8));
}

uint8_t pci_read8(const struct pci_dev *d, uint8_t off)
{
    return (uint8_t)(pci_read32(d, off) >> ((off & 3) * 8));
}

void pci_write32(const struct pci_dev *d, uint8_t off, uint32_t v)
{
    platform_pci_write32(d->bus, d->slot, d->func, off, v);
}

void pci_write16(const struct pci_dev *d, uint8_t off, uint16_t v)
{
    uint32_t old = pci_read32(d, off);
    unsigned shift = (off & 2) * 8;
    old &= ~(0xffffu << shift);
    old |= (uint32_t)v << shift;
    pci_write32(d, off, old);
}

void pci_write8(const struct pci_dev *d, uint8_t off, uint8_t v)
{
    uint32_t old = pci_read32(d, off);
    unsigned shift = (off & 3) * 8;
    old &= ~(0xffu << shift);
    old |= (uint32_t)v << shift;
    pci_write32(d, off, old);
}

/* The size of a BAR: all ones are written to it with decoding off, and
 * the bits that read back as zero below the type bits give the size. This
 * runs at enumeration, before any driver uses the function. */
static uint64_t size_bar(struct pci_dev *d, int i, bool io, bool is64)
{
    uint8_t off = (uint8_t)(0x10 + i * 4);
    uint16_t cmd = pci_read16(d, 0x04);
    pci_write16(d, 0x04, (uint16_t)(cmd & ~3u));          /* I/O and memory decoding off */
    uint32_t lo = pci_read32(d, off);
    pci_write32(d, off, 0xffffffff);
    uint64_t mask = pci_read32(d, off) & (io ? ~3u : ~0xfu);
    pci_write32(d, off, lo);
    if (is64) {
        uint32_t hi = pci_read32(d, (uint8_t)(off + 4));
        pci_write32(d, (uint8_t)(off + 4), 0xffffffff);
        mask |= (uint64_t)pci_read32(d, (uint8_t)(off + 4)) << 32;
        pci_write32(d, (uint8_t)(off + 4), hi);
    } else {
        mask |= io ? 0xffffffffffff0000UL : 0xffffffff00000000UL;
    }
    pci_write16(d, 0x04, cmd);
    return mask ? ~mask + 1 : 0;
}

static void read_bars(struct pci_dev *d)
{
    for (int i = 0; i < 6; i++) {
        uint32_t lo = pci_read32(d, (uint8_t)(0x10 + i * 4));
        if (lo == 0)
            continue;
        if (lo & 1) {
            d->bar[i] = lo & ~3u;
            d->bar_is_io[i] = true;
            d->bar_size[i] = size_bar(d, i, true, false);
            continue;
        }
        uint64_t base = lo & ~0xfu;
        d->bar_prefetchable[i] = lo & 8;
        if (((lo >> 1) & 3) == 2 && i < 5) {
            base |= (uint64_t)pci_read32(d, (uint8_t)(0x10 + (i + 1) * 4)) << 32;
            d->bar[i] = base;
            d->bar_is_64[i] = true;
            d->bar_size[i] = size_bar(d, i, false, true);
            i++;
            continue;
        }
        d->bar[i] = base;
        d->bar_size[i] = size_bar(d, i, false, false);
    }
}

/* Names for the boot log only. Drivers match on the numeric ids. A
 * function is described by its device name when known, else by its class. */
static const char *class_name(uint8_t class, uint8_t subclass)
{
    switch ((unsigned)class << 8 | subclass) {
    case 0x0100: return "scsi controller";
    case 0x0101: return "ide controller";
    case 0x0106: return "sata controller";
    case 0x0180: return "storage controller";
    case 0x0200: return "ethernet controller";
    case 0x0300: return "vga display";
    case 0x0380: return "display controller";
    case 0x0400: return "video device";
    case 0x0401: return "audio device";
    case 0x0403: return "hd audio";
    case 0x0500: return "memory controller";
    case 0x0600: return "host bridge";
    case 0x0601: return "isa bridge";
    case 0x0604: return "pci bridge";
    case 0x0680: return "bridge";
    case 0x0700: return "serial controller";
    case 0x0780: return "communication device";
    case 0x0880: return "system peripheral";
    case 0x0900: return "keyboard controller";
    case 0x0902: return "mouse controller";
    case 0x0c03: return "usb controller";
    case 0x0c05: return "smbus controller";
    case 0x00ff: return "unclassified device";
    default:     return NULL;
    }
}

static const char *device_name(uint16_t vendor, uint16_t device)
{
    if (vendor == 0x1af4) {
        switch (device) {
        case 0x1000: return "virtio-net (legacy)";
        case 0x1001: return "virtio-blk (legacy)";
        case 0x1003: return "virtio-console (legacy)";
        case 0x1005: return "virtio-rng (legacy)";
        case 0x1041: return "virtio-net";
        case 0x1042: return "virtio-blk";
        case 0x1043: return "virtio-console";
        case 0x1044: return "virtio-rng";
        case 0x1050: return "virtio-gpu";
        case 0x1052: return "virtio-input";
        case 0x1059: return "virtio-snd";
        default:     return NULL;
        }
    }
    if (vendor == 0x8086) {
        switch (device) {
        case 0x100e: return "82540EM e1000";
        case 0x10d3: return "82574L e1000e";
        case 0x1237: return "440FX host bridge";
        case 0x29c0: return "82G33 host bridge";
        case 0x2918: return "ICH9 lpc";
        case 0x2922: return "ICH9 ahci";
        case 0x2930: return "ICH9 smbus";
        case 0x7000: return "PIIX3 isa";
        case 0x7010: return "PIIX3 ide";
        case 0x7113: return "PIIX4 acpi";
        default:     return NULL;
        }
    }
    if (vendor == 0x1234 && device == 0x1111)
        return "qemu vga";
    return NULL;
}

static void probe(uint8_t bus, uint8_t slot, uint8_t func)
{
    uint32_t id = raw_read32(bus, slot, func, 0);
    if ((id & 0xffff) == 0xffff || ndevices == PCI_MAX_DEVICES)
        return;
    struct pci_dev *d = &devices[ndevices++];
    d->bus = bus;
    d->slot = slot;
    d->func = func;
    d->vendor = (uint16_t)id;
    d->device = (uint16_t)(id >> 16);
    uint32_t cls = pci_read32(d, 0x08);
    d->class = (uint8_t)(cls >> 24);
    d->subclass = (uint8_t)(cls >> 16);
    d->prog_if = (uint8_t)(cls >> 8);
    d->revision = (uint8_t)cls;
    d->header_type = pci_read8(d, 0x0e) & 0x7f;
    uint32_t irq = pci_read32(d, 0x3c);
    d->irq_line = (uint8_t)irq;
    d->irq_pin = (uint8_t)(irq >> 8);
    if (d->header_type == 0) {
        read_bars(d);
        uint32_t sub = pci_read32(d, 0x2c);
        d->subsystem_vendor = (uint16_t)sub;
        d->subsystem_device = (uint16_t)(sub >> 16);
    }
    unsigned nbars = 0;
    for (int i = 0; i < 6; i++)
        nbars += d->bar[i] != 0;
    const char *what = device_name(d->vendor, d->device);
    char clsbuf[16];
    if (!what)
        what = class_name(d->class, d->subclass);
    if (!what) {
        ksnprintf(clsbuf, sizeof clsbuf, "class %02x%02x", d->class, d->subclass);
        what = clsbuf;
    }
    if (d->irq_pin)
        klog_info("%02x:%02x.%u %04x:%04x %s, %u bar%s, int%c line %u", bus, slot, func,
                  d->vendor, d->device, what, nbars, nbars == 1 ? "" : "s",
                  'A' + d->irq_pin - 1, d->irq_line);
    else
        klog_info("%02x:%02x.%u %04x:%04x %s, %u bar%s, no interrupt pin", bus, slot, func,
                  d->vendor, d->device, what, nbars, nbars == 1 ? "" : "s");
}

/* Scan bus 0 and every bus that a bridge found earlier names as its
 * secondary bus, in bus order. The firmware numbers the buses below a
 * bridge after the bridge's own, so one pass in order reaches them all
 * without reading the configuration space of absent buses, which on ECAM
 * platforms would have to be mapped. */
void pci_init(void)
{
    uint64_t reachable[4] = { 1 };
    unsigned nbuses = 0;
    for (unsigned bus = 0; bus < 256; bus++) {
        if (!(reachable[bus / 64] & (1UL << (bus % 64))))
            continue;
        bool populated = false;
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint32_t id = raw_read32((uint8_t)bus, slot, 0, 0);
            if ((id & 0xffff) == 0xffff)
                continue;
            populated = true;
            uint8_t ht = (uint8_t)(raw_read32((uint8_t)bus, slot, 0, 0x0c) >> 16);
            uint8_t nfunc = (ht & 0x80) ? 8 : 1;
            for (uint8_t f = 0; f < nfunc; f++) {
                probe((uint8_t)bus, slot, f);
                uint32_t hdr = raw_read32((uint8_t)bus, slot, f, 0x0c);
                if (((hdr >> 16) & 0x7f) != 1 || raw_read32((uint8_t)bus, slot, f, 0) == 0xffffffff)
                    continue;
                /* A PCI to PCI bridge: the secondary bus is byte 0x19. */
                unsigned secondary = (raw_read32((uint8_t)bus, slot, f, 0x18) >> 8) & 0xff;
                if (secondary > bus)
                    reachable[secondary / 64] |= 1UL << (secondary % 64);
            }
        }
        nbuses += populated;
    }
    klog_info("%zu functions on %u bus%s", ndevices, nbuses,
              nbuses == 1 ? "" : "es");
}

size_t pci_count(void)
{
    return ndevices;
}

struct pci_dev *pci_device(size_t index)
{
    return index < ndevices ? &devices[index] : NULL;
}

struct pci_dev *pci_find(uint16_t vendor, uint16_t device)
{
    for (size_t i = 0; i < ndevices; i++)
        if (devices[i].vendor == vendor && devices[i].device == device)
            return &devices[i];
    return NULL;
}

uint8_t pci_find_capability(const struct pci_dev *d, uint8_t id)
{
    if (!(pci_read16(d, 0x06) & (1 << 4)))
        return 0;
    uint8_t off = pci_read8(d, 0x34) & 0xfc;
    for (int guard = 0; off && guard < 48; guard++) {
        if (pci_read8(d, off) == id)
            return off;
        off = pci_read8(d, (uint8_t)(off + 1)) & 0xfc;
    }
    return 0;
}

void pci_enable_bus_master(const struct pci_dev *d)
{
    uint16_t cmd = pci_read16(d, 0x04);
    cmd |= (1 << 1) | (1 << 2);     /* memory space, bus master */
    pci_write16(d, 0x04, cmd);
}

/* The CPU that receives the next MSI-X vector: the CPUs that have started,
 * in turn, so that the interrupts of the devices are spread over them.
 * Devices set up during boot, before the application processors start,
 * use the boot CPU. */
static unsigned next_msi_cpu;           /* advanced atomically */

static unsigned pick_msi_cpu(void)
{
    unsigned n = smp_cpu_count();
    for (unsigned i = 0; i < n; i++) {
        unsigned c = __atomic_fetch_add(&next_msi_cpu, 1, __ATOMIC_RELAXED) % n;
        if (__atomic_load_n(&cpu_by_id(c)->started, __ATOMIC_ACQUIRE))
            return c;
    }
    return cpu_current()->id;
}

/* MSI-X table, mapped on first use. */
static volatile uint32_t *msix_table(const struct pci_dev *d, uint8_t cap, unsigned *nentries)
{
    uint32_t tbl = pci_read32(d, (uint8_t)(cap + 4));
    unsigned bir = tbl & 7;
    uint64_t off = tbl & ~7u;
    *nentries = (pci_read16(d, (uint8_t)(cap + 2)) & 0x7ff) + 1;
    uint64_t pa = d->bar[bir] + off;
    size_t size = ALIGN_UP((pa & (PAGE_SIZE - 1)) + *nentries * 16, PAGE_SIZE);
    volatile uint8_t *va = vmm_map_mmio(pa & PAGE_MASK, size, VM_READ | VM_WRITE | VM_NOCACHE);
    if (!va)
        return NULL;
    return (volatile uint32_t *)(va + (pa & (PAGE_SIZE - 1)));
}

int pci_msix_enable(const struct pci_dev *d)
{
    uint8_t cap = pci_find_capability(d, PCI_CAP_MSIX);
    if (!cap)
        return -ENODEV;
    uint16_t ctl = pci_read16(d, (uint8_t)(cap + 2));
    ctl |= 1 << 15;         /* enable */
    ctl &= ~(1 << 14);      /* clear function mask */
    pci_write16(d, (uint8_t)(cap + 2), ctl);
    return 0;
}

int pci_msix_set_vector(const struct pci_dev *d, unsigned index, unsigned vector)
{
    uint8_t cap = pci_find_capability(d, PCI_CAP_MSIX);
    if (!cap)
        return -ENODEV;
    unsigned n;
    volatile uint32_t *tbl = msix_table(d, cap, &n);
    if (!tbl)
        return -ENOMEM;
    if (index >= n)
        return -EINVAL;
    uint64_t addr;
    uint32_t data;
    platform_msi_compose(d, vector, pick_msi_cpu(), &addr, &data);
    volatile uint32_t *e = tbl + index * 4;
    e[0] = (uint32_t)addr;
    e[1] = (uint32_t)(addr >> 32);
    e[2] = data;
    e[3] = 0;               /* unmask */
    return 0;
}

uint64_t pci_bar_size(const struct pci_dev *d, int bar)
{
    return bar >= 0 && bar <= 5 ? d->bar_size[bar] : 0;
}

/* MSI with one message. The capability has a 32 or 64 bit address, and the
 * data register follows it. INTx is disabled while MSI is on. */
int pci_msi_enable(const struct pci_dev *d, unsigned vector)
{
    uint8_t cap = pci_find_capability(d, PCI_CAP_MSI);
    if (!cap)
        return -ENODEV;
    uint16_t ctl = pci_read16(d, (uint8_t)(cap + 2));
    bool is64 = ctl & (1 << 7);
    uint64_t addr;
    uint32_t data;
    platform_msi_compose(d, vector, pick_msi_cpu(), &addr, &data);
    pci_write32(d, (uint8_t)(cap + 4), (uint32_t)addr);
    if (is64) {
        pci_write32(d, (uint8_t)(cap + 8), (uint32_t)(addr >> 32));
        pci_write16(d, (uint8_t)(cap + 12), (uint16_t)data);
    } else {
        pci_write16(d, (uint8_t)(cap + 8), (uint16_t)data);
    }
    ctl &= ~(7 << 4);       /* one message */
    ctl |= 1;               /* enable */
    pci_write16(d, (uint8_t)(cap + 2), ctl);
    pci_write16(d, 0x04, (uint16_t)(pci_read16(d, 0x04) | (1 << 10)));    /* INTx off */
    return 0;
}

/* ---- /dev/devices (docs/design/sysinfo.md) ---- */

static const char *vendor_name(uint16_t vendor)
{
    switch (vendor) {
    case 0x1000: return "Broadcom / LSI";
    case 0x1002: return "AMD (ATI)";
    case 0x1013: return "Cirrus Logic";
    case 0x1022: return "AMD";
    case 0x1033: return "NEC";
    case 0x104c: return "Texas Instruments";
    case 0x106b: return "Apple";
    case 0x10de: return "NVIDIA";
    case 0x10ec: return "Realtek";
    case 0x1106: return "VIA";
    case 0x1234: return "QEMU (Bochs)";
    case 0x1414: return "Microsoft";
    case 0x144d: return "Samsung";
    case 0x14e4: return "Broadcom";
    case 0x15ad: return "VMware";
    case 0x168c: return "Qualcomm Atheros";
    case 0x1912: return "Renesas";
    case 0x19e5: return "Huawei";
    case 0x1af4: return "Red Hat (virtio)";
    case 0x1b21: return "ASMedia";
    case 0x1b36: return "Red Hat (QEMU)";
    case 0x1d0f: return "Amazon";
    case 0x5853: return "XenSource";
    case 0x8086: return "Intel";
    case 0x80ee: return "Oracle (VirtualBox)";
    default:     return "unknown";
    }
}

/* Names of devices that the boot log does not name (device_name). */
static const char *more_device_names(uint16_t vendor, uint16_t device)
{
    if (vendor == 0x1af4) {
        switch (device) {
        case 0x1002: return "virtio-balloon (legacy)";
        case 0x1004: return "virtio-scsi (legacy)";
        case 0x1009: return "virtio-9p (legacy)";
        case 0x1045: return "virtio-balloon";
        case 0x1048: return "virtio-scsi";
        case 0x1049: return "virtio-9p";
        case 0x1053: return "virtio-vsock";
        default:     return NULL;
        }
    }
    if (vendor == 0x1b36) {
        switch (device) {
        case 0x0001: return "QEMU PCI to PCI bridge";
        case 0x0008: return "QEMU PCIe host bridge";
        case 0x000c: return "QEMU PCIe root port";
        case 0x000d: return "QEMU xHCI controller";
        case 0x0010: return "QEMU NVMe controller";
        default:     return NULL;
        }
    }
    if (vendor == 0x1033 && device == 0x0194)
        return "uPD720200 xHCI controller";
    if (vendor == 0x8086) {
        switch (device) {
        case 0x2668: return "ICH6 HD audio";
        case 0x293e: return "ICH9 HD audio";
        default:     return NULL;
        }
    }
    return NULL;
}

/* The class alone, for a subclass without a name. */
static const char *base_class_name(uint8_t class)
{
    static const char *const names[] = {
        "unclassified", "mass storage controller", "network controller", "display controller",
        "multimedia controller", "memory controller", "bridge", "communication controller",
        "system peripheral", "input device controller", "docking station", "processor",
        "serial bus controller", "wireless controller", "intelligent controller", "satellite controller",
        "encryption controller", "signal processing controller", "processing accelerator",
        "non-essential instrumentation",
    };
    return class < sizeof names / sizeof names[0] ? names[class] : "unassigned class";
}

static const char *interface_name(uint8_t class, uint8_t subclass, uint8_t prog_if)
{
    if (class == 0x0c && subclass == 0x03) {
        switch (prog_if) {
        case 0x00: return "UHCI";
        case 0x10: return "OHCI";
        case 0x20: return "EHCI";
        case 0x30: return "xHCI";
        case 0xfe: return "USB device";
        default:   return NULL;
        }
    }
    if (class == 0x01 && subclass == 0x06 && prog_if == 0x01)
        return "AHCI";
    if (class == 0x01 && subclass == 0x08 && prog_if == 0x02)
        return "NVM Express";
    if (class == 0x03 && subclass == 0x00 && prog_if == 0x00)
        return "VGA compatible";
    return NULL;
}

static const char *capability_name(uint8_t id)
{
    switch (id) {
    case 0x01: return "power management";
    case 0x02: return "AGP";
    case 0x03: return "vital product data";
    case 0x04: return "slot identification";
    case 0x05: return "MSI";
    case 0x06: return "CompactPCI hot swap";
    case 0x07: return "PCI-X";
    case 0x08: return "HyperTransport";
    case 0x09: return "vendor specific";
    case 0x0a: return "debug port";
    case 0x0c: return "PCI hot plug";
    case 0x0d: return "bridge subsystem vendor";
    case 0x0e: return "AGP 8x";
    case 0x0f: return "secure device";
    case 0x10: return "PCI Express";
    case 0x11: return "MSI-X";
    case 0x12: return "SATA";
    case 0x13: return "advanced features";
    case 0x14: return "enhanced allocation";
    default:   return "unknown";
    }
}

static const char *pcie_type(unsigned t)
{
    switch (t) {
    case 0x0: return "endpoint";
    case 0x1: return "legacy endpoint";
    case 0x4: return "root port";
    case 0x5: return "switch upstream port";
    case 0x6: return "switch downstream port";
    case 0x7: return "PCI Express to PCI bridge";
    case 0x8: return "PCI to PCI Express bridge";
    case 0x9: return "root complex integrated endpoint";
    case 0xa: return "root complex event collector";
    default:  return "unknown";
    }
}

static const char *link_speed(unsigned s)
{
    static const char *const speeds[] = { "unknown", "2.5 GT/s", "5 GT/s", "8 GT/s", "16 GT/s", "32 GT/s", "64 GT/s" };
    return s < sizeof speeds / sizeof speeds[0] ? speeds[s] : "unknown";
}

static void describe_capabilities(struct devinfo *di, const struct pci_dev *d)
{
    if (!(pci_read16(d, 0x06) & (1 << 4)))
        return;
    char list[256], entry[48];
    list[0] = '\0';
    uint8_t off = pci_read8(d, 0x34) & 0xfc;
    for (int guard = 0; off && guard < 48; guard++) {
        uint8_t id = pci_read8(d, off);
        ksnprintf(entry, sizeof entry, "%s at 0x%02x", capability_name(id), off);
        devinfo_append(list, sizeof list, ", ", entry);
        uint16_t ctl = pci_read16(d, (uint8_t)(off + 2));
        switch (id) {
        case 0x01: {
            static const char *const states[] = { "D0", "D1", "D2", "D3hot" };
            devinfo_prop(di, "power_management", "version %u, state %s", ctl & 7,
                         states[pci_read16(d, (uint8_t)(off + 4)) & 3]);
            break;
        }
        case 0x05:
            devinfo_prop(di, "msi", "%s, %u of %u vectors, %s addresses%s", (ctl & 1) ? "enabled" : "disabled",
                         1u << ((ctl >> 4) & 7), 1u << ((ctl >> 1) & 7), (ctl & (1 << 7)) ? "64 bit" : "32 bit",
                         (ctl & (1 << 8)) ? ", per vector masking" : "");
            break;
        case 0x11: {
            uint32_t table = pci_read32(d, (uint8_t)(off + 4)), pba = pci_read32(d, (uint8_t)(off + 8));
            devinfo_prop(di, "msi_x", "%s%s, %u vectors, table in BAR %u at 0x%x, pending bits in BAR %u at 0x%x",
                         (ctl & (1 << 15)) ? "enabled" : "disabled", (ctl & (1 << 14)) ? ", masked" : "",
                         (ctl & 0x7ff) + 1u, table & 7, table & ~7u, pba & 7, pba & ~7u);
            break;
        }
        case 0x10: {
            unsigned type = (ctl >> 4) & 0xf;
            devinfo_prop(di, "pcie", "version %u, %s", ctl & 0xf, pcie_type(type));
            if (type != 0x9 && type != 0xa) {
                uint32_t lcap = pci_read32(d, (uint8_t)(off + 0x0c));
                uint16_t lsta = pci_read16(d, (uint8_t)(off + 0x12));
                devinfo_prop(di, "pcie_link", "%s x%u, maximum %s x%u", link_speed(lsta & 0xf), (lsta >> 4) & 0x3f,
                             link_speed(lcap & 0xf), (lcap >> 4) & 0x3f);
            }
            break;
        }
        default:
            break;
        }
        off = pci_read8(d, (uint8_t)(off + 1)) & 0xfc;
    }
    devinfo_prop(di, "capabilities", "%s", list);
}

static void describe_bar(struct devinfo *di, const struct pci_dev *d, int i)
{
    char key[8], size[32];
    ksnprintf(key, sizeof key, "bar%d", i);
    devinfo_format_size(size, sizeof size, d->bar_size[i]);
    if (d->bar_is_io[i])
        devinfo_prop(di, key, "I/O ports 0x%lx, %s", (unsigned long)d->bar[i], size);
    else
        devinfo_prop(di, key, "memory 0x%lx, %s, %s%s", (unsigned long)d->bar[i], size,
                     d->bar_is_64[i] ? "64 bit" : "32 bit", d->bar_prefetchable[i] ? ", prefetchable" : "");
}

void pci_describe(struct devinfo *di)
{
    devinfo_node(di, "pci", "PCI");
    devinfo_prop(di, "functions", "%zu", ndevices);
    for (size_t i = 0; i < ndevices; i++) {
        const struct pci_dev *d = &devices[i];
        const char *dev = device_name(d->vendor, d->device);
        if (!dev)
            dev = more_device_names(d->vendor, d->device);
        const char *cls = class_name(d->class, d->subclass);
        if (!cls)
            cls = base_class_name(d->class);
        char path[24];
        ksnprintf(path, sizeof path, "pci/%02x:%02x.%u", d->bus, d->slot, d->func);
        devinfo_node(di, path, "%02x:%02x.%u %s", d->bus, d->slot, d->func, dev ? dev : cls);
        devinfo_prop(di, "address", "%02x:%02x.%u", d->bus, d->slot, d->func);
        devinfo_prop(di, "vendor", "%04x (%s)", d->vendor, vendor_name(d->vendor));
        devinfo_prop(di, "device", "%04x%s%s%s", d->device, dev ? " (" : "", dev ? dev : "", dev ? ")" : "");
        if (d->subsystem_vendor || d->subsystem_device)
            devinfo_prop(di, "subsystem", "%04x:%04x (%s)", d->subsystem_vendor, d->subsystem_device,
                         vendor_name(d->subsystem_vendor));
        const char *intf = interface_name(d->class, d->subclass, d->prog_if);
        devinfo_prop(di, "class", "%02x%02x%02x, %s%s%s", d->class, d->subclass, d->prog_if, cls, intf ? ", " : "",
                     intf ? intf : "");
        devinfo_prop(di, "revision", "0x%02x", d->revision);
        devinfo_prop(di, "header_type", "%u%s", d->header_type,
                     d->header_type == 1 ? " (PCI to PCI bridge)" : d->header_type == 2 ? " (CardBus bridge)" : "");
        devinfo_prop(di, "driver", "%s", d->driver ? d->driver : "none");
        uint16_t cmd = pci_read16(d, 0x04);
        devinfo_prop(di, "command", "0x%04x (%s%s%s%s)", cmd, (cmd & 1) ? "I/O " : "", (cmd & 2) ? "memory " : "",
                     (cmd & 4) ? "bus master " : "", (cmd & (1 << 10)) ? "INTx off" : "INTx on");
        for (int b = 0; b < 6; b++)
            if (d->bar[b])
                describe_bar(di, d, b);
        if (d->header_type == 1) {
            uint32_t buses = pci_read32(d, 0x18);
            devinfo_prop(di, "buses", "primary %u, secondary %u, subordinate %u", buses & 0xff, (buses >> 8) & 0xff,
                         (buses >> 16) & 0xff);
        }
        if (d->irq_pin)
            devinfo_prop(di, "interrupt_pin", "INT%c, line %u", 'A' + d->irq_pin - 1, d->irq_line);
        else
            devinfo_prop(di, "interrupt_pin", "none");
        describe_capabilities(di, d);
    }
}
