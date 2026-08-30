#pragma once
#include <kernel.h>
#include <lib/list.h>

#define PCI_MAX_DEVICES 64

/* One PCI function found during enumeration. The table is filled once at
 * boot and read only afterwards, so no lock is needed. */
struct pci_dev {
    uint8_t bus, slot, func;
    uint16_t vendor, device;
    uint8_t class, subclass, prog_if;
    uint8_t header_type;
    uint8_t irq_line, irq_pin;
    uint64_t bar[6];            /* base address, 0 if unused */
    bool bar_is_io[6];
};

void pci_init(void);
size_t pci_count(void);
struct pci_dev *pci_device(size_t index);
struct pci_dev *pci_find(uint16_t vendor, uint16_t device);

uint32_t pci_read32(const struct pci_dev *dev, uint8_t off);
uint16_t pci_read16(const struct pci_dev *dev, uint8_t off);
uint8_t pci_read8(const struct pci_dev *dev, uint8_t off);
void pci_write32(const struct pci_dev *dev, uint8_t off, uint32_t v);
void pci_write16(const struct pci_dev *dev, uint8_t off, uint16_t v);
void pci_write8(const struct pci_dev *dev, uint8_t off, uint8_t v);

/* Walk the capability list for id. Returns the offset or 0. */
uint8_t pci_find_capability(const struct pci_dev *dev, uint8_t id);
void pci_enable_bus_master(const struct pci_dev *dev);

/* MSI-X: enable the table and point entry index at vector on this CPU. */
int pci_msix_enable(const struct pci_dev *dev);
int pci_msix_set_vector(const struct pci_dev *dev, unsigned index, uint8_t vector);

#define PCI_CAP_MSIX   0x11
#define PCI_CAP_VENDOR 0x09
