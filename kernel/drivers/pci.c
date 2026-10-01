#define KLOG_SUBSYS "pci"
#include <drivers/pci.h>
#include <arch/platform.h>
#include <mm/vmm.h>
#include <klog.h>
#include <lib/printf.h>
#include <errno.h>

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

static void read_bars(struct pci_dev *d)
{
    for (int i = 0; i < 6; i++) {
        uint32_t lo = pci_read32(d, (uint8_t)(0x10 + i * 4));
        if (lo == 0)
            continue;
        if (lo & 1) {
            d->bar[i] = lo & ~3u;
            d->bar_is_io[i] = true;
            continue;
        }
        uint64_t base = lo & ~0xfu;
        if (((lo >> 1) & 3) == 2 && i < 5) {
            base |= (uint64_t)pci_read32(d, (uint8_t)(0x10 + (i + 1) * 4)) << 32;
            d->bar[i] = base;
            i++;
            continue;
        }
        d->bar[i] = base;
    }
}

/* Names for the boot log only; drivers match on the numeric ids. A
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
    d->header_type = pci_read8(d, 0x0e) & 0x7f;
    uint32_t irq = pci_read32(d, 0x3c);
    d->irq_line = (uint8_t)irq;
    d->irq_pin = (uint8_t)(irq >> 8);
    if (d->header_type == 0)
        read_bars(d);
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

void pci_init(void)
{
    unsigned nbuses = 0;
    for (unsigned bus = 0; bus < 256; bus++) {
        bool populated = false;
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint32_t id = raw_read32((uint8_t)bus, slot, 0, 0);
            if ((id & 0xffff) == 0xffff)
                continue;
            populated = true;
            uint8_t ht = (uint8_t)(raw_read32((uint8_t)bus, slot, 0, 0x0c) >> 16);
            uint8_t nfunc = (ht & 0x80) ? 8 : 1;
            for (uint8_t f = 0; f < nfunc; f++)
                probe((uint8_t)bus, slot, f);
        }
        nbuses += populated;
    }
    klog_info("%zu functions on %u bus%s, config space through ports cf8/cfc", ndevices, nbuses,
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
    platform_msi_compose(vector, &addr, &data);
    volatile uint32_t *e = tbl + index * 4;
    e[0] = (uint32_t)addr;
    e[1] = (uint32_t)(addr >> 32);
    e[2] = data;
    e[3] = 0;               /* unmask */
    return 0;
}
