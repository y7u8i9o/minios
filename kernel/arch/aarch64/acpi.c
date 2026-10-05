#define KLOG_SUBSYS "acpi"
#include "acpi.h"
#include "devtree.h"
#include <boot.h>
#include <klog.h>
#include <lib/string.h>
#include <mm/memlayout.h>
#include <arch/irq.h>
#include <uacpi/uacpi.h>
#include <uacpi/tables.h>
#include <uacpi/acpi.h>
#include <uacpi/kernel_api.h>

/* The ACPI tables of an aarch64 machine without a device tree (D1,
 * docs/design/acpi.md). uACPI reads the tables through its early table
 * access. This file fills struct devtree from the tables. Everything here
 * runs once on the boot CPU in devtree_init, before vmm_init, while the
 * page tables of Limine are active. The kernel functions of uACPI are in
 * drivers/acpi_kernel.c. */

/* The IORT (IO Remapping Table, Arm DEN 0049). uACPI does not define it.
 * Only the parts that translate PCI requester IDs to ITS device IDs are
 * read: the root complex node, its ID mappings and the ITS group node. */
#define IORT_NODE_ITS_GROUP    0
#define IORT_NODE_ROOT_COMPLEX 2

struct iort {
    struct acpi_sdt_hdr hdr;
    uint32_t node_count;
    uint32_t node_offset;
    uint32_t reserved;
} __packed;

struct iort_node {
    uint8_t type;
    uint16_t length;
    uint8_t revision;
    uint32_t identifier;
    uint32_t mapping_count;
    uint32_t mapping_offset;
} __packed;

struct iort_id_mapping {
    uint32_t input_base;
    uint32_t id_count;          /* the number of IDs in the range minus one */
    uint32_t output_base;
    uint32_t output_reference;  /* offset of the target node from the start of the table */
    uint32_t flags;
} __packed;

/* The node at offset off of the table, or NULL outside the table. */
static const struct iort_node *iort_node_at(const struct iort *t, uint32_t off)
{
    if (off < sizeof *t || off + sizeof(struct iort_node) > t->hdr.length)
        return NULL;
    const struct iort_node *n = (const struct iort_node *)((const uint8_t *)t + off);
    if (n->length < sizeof *n || off + n->length > t->hdr.length)
        return NULL;
    return n;
}

/* The first ID mapping of the first root complex that leads to an ITS
 * group directly. Without an IORT, or without such a mapping, the device
 * ID is the requester ID, as on QEMU virt. */
static void read_iort(void)
{
    uacpi_table tbl;
    if (uacpi_table_find_by_signature("IORT", &tbl) != UACPI_STATUS_OK) {
        klog_info("no IORT, ITS device IDs are the PCI requester IDs");
        return;
    }
    const struct iort *t = tbl.ptr;
    uint32_t off = t->node_offset;
    for (uint32_t i = 0; i < t->node_count; i++) {
        const struct iort_node *n = iort_node_at(t, off);
        if (!n)
            break;
        off += n->length;
        if (n->type != IORT_NODE_ROOT_COMPLEX)
            continue;
        for (uint32_t m = 0; m < n->mapping_count; m++) {
            uint32_t moff = n->mapping_offset + m * (uint32_t)sizeof(struct iort_id_mapping);
            if (moff + sizeof(struct iort_id_mapping) > n->length)
                break;
            const struct iort_id_mapping *map =
                (const struct iort_id_mapping *)((const uint8_t *)n + moff);
            const struct iort_node *target = iort_node_at(t, map->output_reference);
            if (!target || target->type != IORT_NODE_ITS_GROUP)
                continue;
            devtree.msi_rid_base = map->input_base;
            devtree.msi_base = map->output_base;
            devtree.msi_length = map->id_count + 1;
            uacpi_table_unref(&tbl);
            return;
        }
    }
    klog_warn("IORT has no root complex mapped to an ITS, using the requester IDs");
    uacpi_table_unref(&tbl);
}

/* The interrupt controller from the MADT: the distributor with its
 * version, the GICv2 CPU interface of the first CPU, the GICv2m frame, the
 * GICv3 redistributor region and the ITS. */
struct madt_state {
    bool seen_gicc;
    uint64_t gicc_gicr_min;     /* lowest redistributor address of the GICC entries */
    unsigned gicc_gicr_count;   /* GICC entries with a redistributor address */
};

static uacpi_iteration_decision madt_entry(uacpi_handle user, struct acpi_entry_hdr *e)
{
    struct madt_state *s = user;
    switch (e->type) {
    case ACPI_MADT_ENTRY_TYPE_GICD: {
        const struct acpi_madt_gicd *d = (const struct acpi_madt_gicd *)e;
        devtree.gicd = d->address;
        /* Version 0 leaves the version to the hardware. The GICv3 entries
         * below then decide it. */
        if (d->gic_version == 1 || d->gic_version == 2)
            devtree.gic_version = 2;
        else if (d->gic_version >= 3)
            devtree.gic_version = 3;
        break;
    }
    case ACPI_MADT_ENTRY_TYPE_GICC: {
        const struct acpi_madt_gicc *c = (const struct acpi_madt_gicc *)e;
        if (!s->seen_gicc) {
            devtree.gicc = c->address;
            s->seen_gicc = true;
        }
        /* A GICv3 without a GICR entry names the redistributor of each
         * CPU in its GICC entry. */
        if (c->gicr_base_address) {
            if (!s->gicc_gicr_count || c->gicr_base_address < s->gicc_gicr_min)
                s->gicc_gicr_min = c->gicr_base_address;
            s->gicc_gicr_count++;
        }
        break;
    }
    case ACPI_MADT_ENTRY_TYPE_GIC_MSI_FRAME: {
        const struct acpi_madt_gic_msi_frame *f = (const struct acpi_madt_gic_msi_frame *)e;
        if (!devtree.v2m)
            devtree.v2m = f->address;
        break;
    }
    case ACPI_MADT_ENTRY_TYPE_GICR: {
        const struct acpi_madt_gicr *r = (const struct acpi_madt_gicr *)e;
        if (!devtree.gicr_size) {
            devtree.gicr = r->address;
            devtree.gicr_size = r->length;
        }
        break;
    }
    case ACPI_MADT_ENTRY_TYPE_GIC_ITS: {
        const struct acpi_madt_gic_its *i = (const struct acpi_madt_gic_its *)e;
        if (!devtree.its)
            devtree.its = i->address;
        break;
    }
    default:
        break;
    }
    return UACPI_ITERATION_DECISION_CONTINUE;
}

static bool read_madt(void)
{
    uacpi_table tbl;
    if (uacpi_table_find_by_signature(ACPI_MADT_SIGNATURE, &tbl) != UACPI_STATUS_OK) {
        klog_error("no MADT, the interrupt controller is unknown");
        return false;
    }
    /* The values of virt without a tree are replaced by the table. */
    devtree.gic_version = 0;
    devtree.gicd = devtree.gicr = devtree.gicc = devtree.its = devtree.v2m = 0;
    devtree.gicr_size = 0;
    struct madt_state state = { 0 };
    uacpi_for_each_subtable(tbl.hdr, sizeof(struct acpi_madt), madt_entry, &state);
    uacpi_table_unref(&tbl);
    /* The redistributors of the GICC entries form one region from the
     * lowest address, 128 KiB per CPU on a GICv3. */
    if (!devtree.gicr_size && state.gicc_gicr_count) {
        devtree.gicr = state.gicc_gicr_min;
        devtree.gicr_size = (size_t)state.gicc_gicr_count * 0x20000;
    }
    if (!devtree.gic_version)
        devtree.gic_version = devtree.gicr_size ? 3 : 2;
    if (!devtree.gicd || (devtree.gic_version == 3 && !devtree.gicr_size) ||
        (devtree.gic_version == 2 && !devtree.gicc)) {
        klog_error("MADT describes no usable GICv%u", devtree.gic_version);
        return false;
    }
    if (devtree.gic_version == 2) {
        devtree.gicr = devtree.its = 0;
        devtree.gicr_size = 0;
    }
    return true;
}

/* The ECAM window of PCI segment 0 from the MCFG. The window of the
 * device tree starts at its first bus, and the MCFG address is that of
 * bus 0. */
static void read_mcfg(void)
{
    uacpi_table tbl;
    if (uacpi_table_find_by_signature(ACPI_MCFG_SIGNATURE, &tbl) != UACPI_STATUS_OK) {
        klog_warn("no MCFG, no PCI devices");
        return;
    }
    const struct acpi_mcfg *m = tbl.ptr;
    size_t n = (m->hdr.length - sizeof *m) / sizeof m->entries[0];
    for (size_t i = 0; i < n; i++) {
        const struct acpi_mcfg_allocation *a = &m->entries[i];
        if (a->segment != 0 || a->end_bus < a->start_bus)
            continue;
        devtree.bus_start = a->start_bus;
        devtree.bus_end = a->end_bus;
        devtree.ecam = a->address + ((uint64_t)a->start_bus << 20);
        devtree.ecam_size = (size_t)(a->end_bus - a->start_bus + 1) << 20;
        break;
    }
    uacpi_table_unref(&tbl);
    if (!devtree.ecam)
        klog_warn("MCFG has no window for segment 0, no PCI devices");
}

/* The PSCI conduit from the FADT. */
static void read_fadt(void)
{
    struct acpi_fadt *f;
    if (uacpi_table_fadt(&f) != UACPI_STATUS_OK || !f) {
        klog_warn("no FADT, PSCI through hvc");
        return;
    }
    if (!(f->arm_boot_arch & ACPI_ARM_PSCI_COMPLIANT))
        klog_warn("FADT does not declare PSCI, power off may not work");
    devtree.psci_smc = !(f->arm_boot_arch & ACPI_ARM_PSCI_USE_HVC);
}

/* The kernel takes the virtual timer at IRQ_TIMER, the interrupt the Arm
 * Base System Architecture assigns to it. The GTDT states the interrupt,
 * and another value is reported. */
static void check_gtdt(void)
{
    uacpi_table tbl;
    if (uacpi_table_find_by_signature("GTDT", &tbl) != UACPI_STATUS_OK)
        return;
    const struct acpi_gtdt *g = tbl.ptr;
    if (g->el1_virtual_gsiv != IRQ_TIMER)
        klog_warn("GTDT puts the virtual timer at interrupt %u, the kernel uses %u", g->el1_virtual_gsiv, IRQ_TIMER);
    uacpi_table_unref(&tbl);
}

bool acpi_read_platform(void)
{
    if (!bootinfo.rsdp_phys)
        return false;
    /* The early table access stores its table descriptors in this buffer.
     * The kernel stops using uACPI at the end of this function. */
    static uint8_t early_buffer[4096] __aligned(16);
    uacpi_status st = uacpi_setup_early_table_access(early_buffer, sizeof early_buffer);
    if (st != UACPI_STATUS_OK) {
        klog_error("the tables at RSDP %lx cannot be read: %s", (uintptr_t)bootinfo.rsdp_phys,
                   uacpi_status_to_string(st));
        return false;
    }
    bool ok = read_madt();
    if (ok) {
        read_mcfg();
        if (devtree.gic_version == 3)
            read_iort();
        read_fadt();
        check_gtdt();
    }
    uacpi_state_reset();
    return ok;
}
