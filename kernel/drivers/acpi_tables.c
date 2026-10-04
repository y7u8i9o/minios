/* The list of the ACPI tables for /dev/devices (docs/design/sysinfo.md):
 * the signature, the identifiers, the revision, the length and the address
 * of every table that the XSDT or the RSDT names. The list is read once at
 * start-up on both architectures and does not use uACPI, which only the
 * aarch64 kernel builds. */
#define KLOG_SUBSYS "acpi"
#include <drivers/devinfo.h>
#include <boot.h>
#include <mm/vmm.h>
#include <lib/endian.h>
#include <lib/printf.h>
#include <lib/string.h>
#include <klog.h>

#define MAX_TABLES 64

struct table_info {
    char signature[5];
    char oem_id[7];
    char oem_table_id[9];
    char creator_id[5];
    uint8_t revision;
    uint32_t length, oem_revision, creator_revision;
    uint64_t phys;
};

/* Written once by acpi_tables_init before /dev/devices can be opened. */
static struct table_info tables[MAX_TABLES];
static unsigned ntables;
static unsigned rsdp_revision;
static char rsdp_oem[7];

/* A fixed width field of a table header as a string without trailing
 * spaces. */
static void field(char *out, const uint8_t *in, size_t n)
{
    memcpy(out, in, n);
    out[n] = '\0';
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '\0'))
        out[--n] = '\0';
}

static void add_table(uint64_t phys)
{
    uint8_t hdr[36];
    if (ntables == MAX_TABLES || !vmm_copy_from_phys(hdr, phys, sizeof hdr))
        return;
    struct table_info *t = &tables[ntables++];
    field(t->signature, hdr, 4);
    t->length = get_le32(hdr + 4);
    t->revision = hdr[8];
    field(t->oem_id, hdr + 10, 6);
    field(t->oem_table_id, hdr + 16, 8);
    t->oem_revision = get_le32(hdr + 24);
    field(t->creator_id, hdr + 28, 4);
    t->creator_revision = get_le32(hdr + 32);
    t->phys = phys;
}

void acpi_tables_init(void)
{
    uint8_t rsdp[36];
    if (!bootinfo.rsdp_phys || !vmm_copy_from_phys(rsdp, bootinfo.rsdp_phys, sizeof rsdp) ||
        memcmp(rsdp, "RSD PTR ", 8) != 0)
        return;
    rsdp_revision = rsdp[15];
    field(rsdp_oem, rsdp + 9, 6);
    /* ACPI 2.0 and later name the XSDT with 64 bit entries, ACPI 1.0 the
     * RSDT with 32 bit entries. */
    bool xsdt = rsdp_revision >= 2 && (get_le32(rsdp + 24) || get_le32(rsdp + 28));
    uint64_t root = xsdt ? get_le64(rsdp + 24) : get_le32(rsdp + 16);
    add_table(root);
    if (!ntables || tables[0].length < 36 || tables[0].length > 36 + MAX_TABLES * 8)
        return;
    unsigned entry = xsdt ? 8 : 4, count = (tables[0].length - 36) / entry;
    uint8_t body[MAX_TABLES * 8];
    if (!vmm_copy_from_phys(body, root + 36, count * entry))
        return;
    for (unsigned i = 0; i < count; i++) {
        uint64_t phys = xsdt ? get_le64(body + i * 8)
                             : get_le32(body + i * 4);
        add_table(phys);
        /* The FADT names the DSDT, which no root table lists. */
        struct table_info *t = &tables[ntables - 1];
        if (strcmp(t->signature, "FACP") == 0 && t->length >= 148) {
            uint8_t fadt[148];
            if (vmm_copy_from_phys(fadt, phys, sizeof fadt)) {
                uint64_t dsdt = get_le64(fadt + 140);
                if (!dsdt)
                    dsdt = get_le32(fadt + 40);
                if (dsdt)
                    add_table(dsdt);
            }
        }
    }
    klog_info("%u ACPI tables, ACPI revision %u", ntables, rsdp_revision);
}

void acpi_tables_describe(struct devinfo *d)
{
    if (!ntables)
        return;
    devinfo_node(d, "firmware/acpi", "ACPI tables");
    devinfo_prop(d, "rsdp_revision", "%u (%s)", rsdp_revision, rsdp_revision >= 2 ? "ACPI 2.0 or later" : "ACPI 1.0");
    devinfo_prop(d, "oem_id", "%s", rsdp_oem);
    devinfo_prop(d, "tables", "%u", ntables);
    for (unsigned i = 0; i < ntables; i++) {
        const struct table_info *t = &tables[i];
        char path[40];
        ksnprintf(path, sizeof path, "firmware/acpi/%u", i);
        devinfo_node(d, path, "%s, %u bytes", t->signature, t->length);
        devinfo_prop(d, "signature", "%s", t->signature);
        devinfo_prop(d, "address", "0x%lx", (unsigned long)t->phys);
        devinfo_prop(d, "length", "%u bytes", t->length);
        devinfo_prop(d, "revision", "%u", t->revision);
        devinfo_prop(d, "oem_id", "%s", t->oem_id);
        devinfo_prop(d, "oem_table_id", "%s", t->oem_table_id);
        devinfo_prop(d, "oem_revision", "0x%x", t->oem_revision);
        devinfo_prop(d, "creator", "%s%s0x%x", t->creator_id, t->creator_id[0] ? " " : "", t->creator_revision);
    }
}
