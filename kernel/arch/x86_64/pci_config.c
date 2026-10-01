/* PCI configuration mechanism 1 and MSI messages of the PC. */
#include <arch/platform.h>
#include <arch/io.h>
#include <arch/apic.h>

#define PCI_CONFIG_ADDR 0xcf8
#define PCI_CONFIG_DATA 0xcfc

static uint32_t cfg_addr(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    return 0x80000000u | ((uint32_t)bus << 16) | ((uint32_t)slot << 11) |
           ((uint32_t)func << 8) | (off & 0xfc);
}

uint32_t platform_pci_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off)
{
    outl(PCI_CONFIG_ADDR, cfg_addr(bus, slot, func, off));
    return inl(PCI_CONFIG_DATA);
}

void platform_pci_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t off, uint32_t v)
{
    outl(PCI_CONFIG_ADDR, cfg_addr(bus, slot, func, off));
    outl(PCI_CONFIG_DATA, v);
}

void platform_msi_compose(unsigned irq, uint64_t *addr, uint32_t *data)
{
    /* Fixed delivery to the local APIC of the calling CPU, edge triggered:
     * the interrupt number is the IDT vector. */
    *addr = 0xfee00000u | (lapic_id() << 12);
    *data = irq;
}
