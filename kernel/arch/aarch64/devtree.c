#define KLOG_SUBSYS "dt"
#include "devtree.h"
#include "acpi.h"
#include <boot.h>
#include <lib/fdt.h>
#include <klog.h>
#include <lib/string.h>

/* The addresses of QEMU virt (hw/arm/virt.c), used without a tree. */
struct devtree devtree = {
    .gic_version = 3,
    .gicd = 0x08000000,
    .gicr = 0x080a0000,
    .gicr_size = 0xf60000,
    .its = 0,
    .ecam = 0,
    .rtc = 0x09010000,
    .msi_length = 0x10000,
};

/* Entry index of the reg property of the first node compatible with
 * compat. */
static bool find_reg(const void *blob, const char *compat, int index, uint64_t *addr, uint64_t *size)
{
    struct fdt_node node = { .offset = -1 };
    return fdt_find_compatible(blob, compat, &node) && fdt_reg(blob, &node, index, addr, size);
}

static void read_pcie(const void *blob)
{
    struct fdt_node node = { .offset = -1 };
    uint64_t addr, size;
    if (!fdt_find_compatible(blob, "pci-host-ecam-generic", &node) ||
        !fdt_reg(blob, &node, 0, &addr, &size))
        return;
    devtree.ecam = addr;
    devtree.ecam_size = size;
    devtree.bus_start = 0;
    devtree.bus_end = (unsigned)(size >> 20) - 1;
    int len;
    const void *v = fdt_prop(blob, &node, "bus-range", &len);
    if (v && len == 8) {
        devtree.bus_start = fdt_cell(v, 0);
        devtree.bus_end = fdt_cell(v, 1);
    }
    if (devtree.bus_end > 255)
        devtree.bus_end = 255;
    /* The first msi-map entry: rid-base, the controller, msi-base, length. */
    v = fdt_prop(blob, &node, "msi-map", &len);
    if (v && len >= 16) {
        devtree.msi_rid_base = fdt_cell(v, 0);
        devtree.msi_base = fdt_cell(v, 2);
        devtree.msi_length = fdt_cell(v, 3);
    }
}

/* The power key: the child of gpio-keys with linux,code 116 (KEY_POWER),
 * whose gpios property gives a PL061 controller and its line. The
 * interrupt of the controller is a shared peripheral interrupt. */
static void read_power_key(const void *blob)
{
    struct fdt_node keys = { .offset = -1 }, key, pl061;
    if (!fdt_find_compatible(blob, "gpio-keys", &keys))
        return;
    key = keys;
    while (fdt_find_property(blob, "linux,code", &key)) {
        int len;
        const void *code = fdt_prop(blob, &key, "linux,code", &len);
        const void *gpios = fdt_prop(blob, &key, "gpios", &len);
        if (!code || fdt_cell(code, 0) != 116 || !gpios || len < 8)
            continue;
        uint64_t addr, size;
        if (!fdt_find_phandle(blob, fdt_cell(gpios, 0), &pl061) || !fdt_is_compatible(blob, &pl061, "arm,pl061") ||
            !fdt_reg(blob, &pl061, 0, &addr, &size))
            return;
        const void *irq = fdt_prop(blob, &pl061, "interrupts", &len);
        if (!irq || len < 12 || fdt_cell(irq, 0) != 0)
            return;
        devtree.pl061 = addr;
        devtree.pl061_irq = 32 + fdt_cell(irq, 1);
        devtree.power_line = fdt_cell(gpios, 1);
        return;
    }
}

/* The PSCI conduit of the psci node: "hvc" or "smc". */
static void read_psci(const void *blob)
{
    static const char *const compat[] = { "arm,psci-1.0", "arm,psci-0.2", "arm,psci" };
    for (size_t i = 0; i < sizeof compat / sizeof compat[0]; i++) {
        struct fdt_node node = { .offset = -1 };
        if (!fdt_find_compatible(blob, compat[i], &node))
            continue;
        int len;
        const char *method = fdt_prop(blob, &node, "method", &len);
        if (method && len >= 4 && strncmp(method, "smc", 3) == 0)
            devtree.psci_smc = true;
        return;
    }
}

static void log_platform(void)
{
    const char *source = devtree.from_acpi ? "acpi" : "device tree";
    if (devtree.gic_version == 2)
        klog_info("%s: gicv2 at %lx and %lx, v2m %s%lx, rtc at %lx", source, devtree.gicd, devtree.gicc,
                  devtree.v2m ? "at " : "", devtree.v2m, devtree.rtc);
    else
        klog_info("%s: gicv3 at %lx and %lx, its %s%lx, rtc at %lx", source, devtree.gicd, devtree.gicr,
                  devtree.its ? "at " : "", devtree.its, devtree.rtc);
    if (devtree.ecam)
        klog_info("pcie ecam at %lx for buses %u to %u, msi requester ids %x to %x as device ids from %x",
                  devtree.ecam, devtree.bus_start, devtree.bus_end, devtree.msi_rid_base,
                  devtree.msi_rid_base + devtree.msi_length - 1, devtree.msi_base);
    klog_info("psci through %s", devtree.psci_smc ? "smc" : "hvc");
}

void devtree_init(void)
{
    const void *blob = bootinfo.dtb;
    if (!blob || !fdt_valid(blob)) {
        if (acpi_read_platform()) {
            devtree.from_acpi = true;
            log_platform();
            return;
        }
        klog_warn("no device tree and no ACPI tables, assuming the devices of QEMU virt without PCI");
        return;
    }
    uint64_t addr, size;
    /* A GICv2 is described by the compatible string of its implementation,
     * with the distributor and the CPU interface as its first two
     * registers. QEMU virt names a GICv2 arm,cortex-a15-gic. */
    static const char *const gicv2[] = { "arm,cortex-a15-gic", "arm,gic-400", "arm,cortex-a9-gic",
                                         "arm,cortex-a7-gic" };
    if (find_reg(blob, "arm,gic-v3", 0, &addr, &size)) {
        devtree.gicd = addr;
        if (find_reg(blob, "arm,gic-v3", 1, &addr, &size)) {
            devtree.gicr = addr;
            devtree.gicr_size = size;
        }
        if (find_reg(blob, "arm,gic-v3-its", 0, &addr, &size))
            devtree.its = addr;
    } else {
        for (size_t i = 0; i < sizeof gicv2 / sizeof gicv2[0]; i++) {
            uint64_t caddr, csize;
            if (find_reg(blob, gicv2[i], 0, &addr, &size) && find_reg(blob, gicv2[i], 1, &caddr, &csize)) {
                devtree.gic_version = 2;
                devtree.gicd = addr;
                devtree.gicc = caddr;
                devtree.gicr = devtree.gicr_size = 0;
                break;
            }
        }
        if (devtree.gic_version == 2 && find_reg(blob, "arm,gic-v2m-frame", 0, &addr, &size))
            devtree.v2m = addr;
    }
    if (find_reg(blob, "arm,pl031", 0, &addr, &size))
        devtree.rtc = addr;
    read_pcie(blob);
    read_psci(blob);
    read_power_key(blob);
    /* The root node is the first node of the structure block. */
    struct fdt_node root = { .offset = 0, .addr_cells = 2, .size_cells = 1 };
    int len;
    const char *v = fdt_prop(blob, &root, "model", &len);
    if (v && len > 0)
        strlcpy(devtree.model, v, MIN((size_t)len + 1, sizeof devtree.model));
    v = fdt_prop(blob, &root, "compatible", &len);
    if (v && len > 0)
        strlcpy(devtree.compatible, v, MIN((size_t)len + 1, sizeof devtree.compatible));
    log_platform();
}
