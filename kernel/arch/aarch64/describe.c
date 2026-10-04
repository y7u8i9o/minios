/* The aarch64 part of /dev/devices (docs/design/sysinfo.md): the
 * processors from MIDR_EL1, MPIDR_EL1 and the ID registers (Arm ARM,
 * chapter D23), the caches from CLIDR_EL1 and CCSIDR_EL1, and the
 * platform as the device tree or the ACPI tables describe it. */
#include <drivers/devinfo.h>
#include <drivers/timer.h>
#include <arch/smp.h>
#include <cpu.h>
#include <arch/cpu.h>
#include <boot.h>
#include <lib/printf.h>
#include <lib/string.h>
#include "devtree.h"
#include "gic.h"

#define READ_SYSREG(name) ({ uint64_t v_; __asm__ volatile("mrs %0, " #name : "=r"(v_)); v_; })

static const char *implementer_name(unsigned id)
{
    switch (id) {
    case 0x41: return "Arm";
    case 0x42: return "Broadcom";
    case 0x43: return "Cavium";
    case 0x46: return "Fujitsu";
    case 0x48: return "HiSilicon";
    case 0x4e: return "NVIDIA";
    case 0x50: return "Applied Micro";
    case 0x51: return "Qualcomm";
    case 0x61: return "Apple";
    case 0xc0: return "Ampere";
    default:   return "unknown";
    }
}

/* The cores of Arm Limited, by part number. */
static const char *arm_part_name(unsigned part)
{
    switch (part) {
    case 0xd03: return "Cortex-A53";
    case 0xd04: return "Cortex-A35";
    case 0xd05: return "Cortex-A55";
    case 0xd07: return "Cortex-A57";
    case 0xd08: return "Cortex-A72";
    case 0xd09: return "Cortex-A73";
    case 0xd0a: return "Cortex-A75";
    case 0xd0b: return "Cortex-A76";
    case 0xd0c: return "Neoverse N1";
    case 0xd0d: return "Cortex-A77";
    case 0xd40: return "Neoverse V1";
    case 0xd41: return "Cortex-A78";
    case 0xd44: return "Cortex-X1";
    case 0xd46: return "Cortex-A510";
    case 0xd47: return "Cortex-A710";
    case 0xd48: return "Cortex-X2";
    case 0xd49: return "Neoverse N2";
    case 0xd4f: return "Neoverse V2";
    default:    return NULL;
    }
}

static void midr_name(uint64_t midr, char *buf, size_t size)
{
    unsigned impl = (unsigned)(midr >> 24) & 0xff, part = (unsigned)(midr >> 4) & 0xfff;
    const char *p = impl == 0x41 ? arm_part_name(part) : NULL;
    if (p)
        ksnprintf(buf, size, "%s %s r%up%u", implementer_name(impl), p, (unsigned)(midr >> 20) & 0xf,
                  (unsigned)midr & 0xf);
    else
        ksnprintf(buf, size, "%s part 0x%03x r%up%u", implementer_name(impl), part, (unsigned)(midr >> 20) & 0xf,
                  (unsigned)midr & 0xf);
}

static unsigned field(uint64_t reg, unsigned shift)
{
    return (unsigned)(reg >> shift) & 0xf;
}

/* The features of the ID registers, named as the Arm ARM names the
 * features (FEAT_x without the prefix, in lower case). */
static void features(char *buf, size_t size)
{
    uint64_t isar0 = READ_SYSREG(id_aa64isar0_el1), isar1 = READ_SYSREG(id_aa64isar1_el1);
    uint64_t pfr0 = READ_SYSREG(id_aa64pfr0_el1), pfr1 = READ_SYSREG(id_aa64pfr1_el1);
    buf[0] = '\0';
    if (field(pfr0, 16) != 0xf)
        devinfo_append(buf, size, " ", field(pfr0, 16) >= 1 ? "fp fp16" : "fp");
    if (field(pfr0, 20) != 0xf)
        devinfo_append(buf, size, " ", field(pfr0, 20) >= 1 ? "asimd asimdhp" : "asimd");
    if (field(isar0, 4) >= 1)
        devinfo_append(buf, size, " ", field(isar0, 4) >= 2 ? "aes pmull" : "aes");
    if (field(isar0, 8) >= 1)
        devinfo_append(buf, size, " ", "sha1");
    if (field(isar0, 12) >= 1)
        devinfo_append(buf, size, " ", field(isar0, 12) >= 2 ? "sha256 sha512" : "sha256");
    if (field(isar0, 16) >= 1)
        devinfo_append(buf, size, " ", "crc32");
    if (field(isar0, 20) >= 2)
        devinfo_append(buf, size, " ", "lse");
    if (field(isar0, 28) >= 1)
        devinfo_append(buf, size, " ", "rdm");
    if (field(isar0, 32) >= 1)
        devinfo_append(buf, size, " ", "sha3");
    if (field(isar0, 36) >= 1)
        devinfo_append(buf, size, " ", "sm3");
    if (field(isar0, 40) >= 1)
        devinfo_append(buf, size, " ", "sm4");
    if (field(isar0, 44) >= 1)
        devinfo_append(buf, size, " ", "dotprod");
    if (field(isar0, 48) >= 1)
        devinfo_append(buf, size, " ", "fhm");
    if (field(isar0, 52) >= 1)
        devinfo_append(buf, size, " ", field(isar0, 52) >= 2 ? "flagm flagm2" : "flagm");
    if (field(isar0, 56) >= 1)
        devinfo_append(buf, size, " ", field(isar0, 56) >= 2 ? "tlbios tlbirange" : "tlbios");
    if (field(isar0, 60) >= 1)
        devinfo_append(buf, size, " ", "rng");
    if (field(isar1, 0) >= 1)
        devinfo_append(buf, size, " ", field(isar1, 0) >= 2 ? "dpb dpb2" : "dpb");
    if (field(isar1, 4) || field(isar1, 8))
        devinfo_append(buf, size, " ", "pauth");
    if (field(isar1, 12) >= 1)
        devinfo_append(buf, size, " ", "jscvt");
    if (field(isar1, 16) >= 1)
        devinfo_append(buf, size, " ", "fcma");
    if (field(isar1, 20) >= 1)
        devinfo_append(buf, size, " ", field(isar1, 20) >= 2 ? "lrcpc lrcpc2" : "lrcpc");
    if (field(isar1, 24) || field(isar1, 28))
        devinfo_append(buf, size, " ", "pauth_generic");
    if (field(isar1, 32) >= 1)
        devinfo_append(buf, size, " ", "frintts");
    if (field(isar1, 36) >= 1)
        devinfo_append(buf, size, " ", "sb");
    if (field(isar1, 44) >= 1)
        devinfo_append(buf, size, " ", "bf16");
    if (field(isar1, 52) >= 1)
        devinfo_append(buf, size, " ", "i8mm");
    if (field(pfr0, 24) >= 1)
        devinfo_append(buf, size, " ", "gic_sysreg");
    if (field(pfr0, 28) >= 1)
        devinfo_append(buf, size, " ", "ras");
    if (field(pfr0, 32) >= 1)
        devinfo_append(buf, size, " ", "sve");
    if (field(pfr0, 48) >= 1)
        devinfo_append(buf, size, " ", "dit");
    if (field(pfr0, 56) >= 1)
        devinfo_append(buf, size, " ", "csv2");
    if (field(pfr0, 60) >= 1)
        devinfo_append(buf, size, " ", "csv3");
    if (field(pfr1, 0) >= 1)
        devinfo_append(buf, size, " ", "bti");
    if (field(pfr1, 4) >= 1)
        devinfo_append(buf, size, " ", "ssbs");
    if (field(pfr1, 8) >= 1)
        devinfo_append(buf, size, " ", field(pfr1, 8) >= 2 ? "mte mte2" : "mte");
    if (field(pfr1, 24) >= 1)
        devinfo_append(buf, size, " ", "sme");
}

static const char *exception_level(unsigned v)
{
    return v == 0 ? "not implemented" : v == 1 ? "AArch64" : v == 2 ? "AArch64 and AArch32" : "unknown";
}

static void describe_caches(struct devinfo *d)
{
    uint64_t clidr = READ_SYSREG(clidr_el1);
    bool ccidx = field(READ_SYSREG(id_aa64mmfr2_el1), 20) >= 1;
    unsigned n = 0;
    for (unsigned level = 1; level <= 7; level++) {
        unsigned ctype = (unsigned)(clidr >> (3 * (level - 1))) & 7;
        if (ctype == 0)
            break;
        /* 1 instruction only, 2 data only, 3 separate, 4 unified. */
        for (unsigned instr = 0; instr < 2; instr++) {
            if ((ctype == 1 && !instr) || ((ctype == 2 || ctype == 4) && instr) || ctype > 4)
                continue;
            __asm__ volatile("msr csselr_el1, %0; isb" : : "r"((uint64_t)((level - 1) << 1 | instr)));
            uint64_t ccsidr = READ_SYSREG(ccsidr_el1);
            unsigned line = 1u << ((ccsidr & 7) + 4);
            unsigned ways = ccidx ? (unsigned)((ccsidr >> 3) & 0x1fffff) + 1 : (unsigned)((ccsidr >> 3) & 0x3ff) + 1;
            unsigned sets = ccidx ? (unsigned)((ccsidr >> 32) & 0xffffff) + 1 : (unsigned)((ccsidr >> 13) & 0x7fff) + 1;
            const char *type = ctype == 4 ? "unified" : instr ? "instruction" : "data";
            uint64_t size = (uint64_t)line * ways * sets;
            char path[24];
            ksnprintf(path, sizeof path, "cpu/cache%u", n++);
            devinfo_node(d, path, "L%u %s cache, %lu KiB", level, type, (unsigned long)(size / 1024));
            devinfo_prop(d, "level", "%u", level);
            devinfo_prop(d, "cache_type", "%s", type);
            devinfo_size(d, "cache_size", size);
            devinfo_prop(d, "ways", "%u", ways);
            devinfo_prop(d, "line_size", "%u bytes", line);
            devinfo_prop(d, "sets", "%u", sets);
        }
    }
    __asm__ volatile("msr csselr_el1, xzr; isb");
}

static void describe_cpus(struct devinfo *d)
{
    uint64_t midr = cpu_current()->arch.midr;
    uint64_t pfr0 = READ_SYSREG(id_aa64pfr0_el1), mmfr0 = READ_SYSREG(id_aa64mmfr0_el1);
    uint64_t mmfr1 = READ_SYSREG(id_aa64mmfr1_el1), mmfr2 = READ_SYSREG(id_aa64mmfr2_el1);
    static const unsigned pa_bits[] = { 32, 36, 40, 42, 44, 48, 52 };
    char name[64];
    midr_name(midr, name, sizeof name);
    devinfo_node(d, "cpu", "%s", name);
    devinfo_prop(d, "model_name", "%s", name);
    devinfo_prop(d, "implementer", "0x%02x (%s)", (unsigned)(midr >> 24) & 0xff,
                 implementer_name((unsigned)(midr >> 24) & 0xff));
    devinfo_prop(d, "part", "0x%03x", (unsigned)(midr >> 4) & 0xfff);
    devinfo_prop(d, "variant", "%u", (unsigned)(midr >> 20) & 0xf);
    devinfo_prop(d, "revision", "%u", (unsigned)midr & 0xf);
    devinfo_prop(d, "midr", "0x%08lx", (unsigned long)midr);
    devinfo_prop(d, "cpus", "%u", smp_cpu_count());
    devinfo_prop(d, "current_el", "EL%lu", (unsigned long)(READ_SYSREG(currentel) >> 2) & 3);
    devinfo_prop(d, "el0", "%s", exception_level(field(pfr0, 0)));
    devinfo_prop(d, "el1", "%s", exception_level(field(pfr0, 4)));
    devinfo_prop(d, "el2", "%s", exception_level(field(pfr0, 8)));
    devinfo_prop(d, "el3", "%s", exception_level(field(pfr0, 12)));
    unsigned parange = field(mmfr0, 0);
    devinfo_prop(d, "physical_address_bits", "%u", parange < 7 ? pa_bits[parange] : 0);
    devinfo_prop(d, "virtual_address_bits", "%u", field(mmfr2, 16) >= 1 ? 52 : 48);
    devinfo_prop(d, "asid_bits", "%u", field(mmfr0, 4) == 2 ? 16 : 8);
    char granules[16] = "";
    if (field(mmfr0, 28) != 0xf)
        devinfo_append(granules, sizeof granules, " ", "4K");
    if (field(mmfr0, 20))
        devinfo_append(granules, sizeof granules, " ", "16K");
    if (field(mmfr0, 24) != 0xf)
        devinfo_append(granules, sizeof granules, " ", "64K");
    devinfo_prop(d, "granules", "%s", granules);
    devinfo_prop(d, "hardware_access_flag", "%s",
                 field(mmfr1, 0) >= 2 ? "access flag and dirty state" : field(mmfr1, 0) == 1 ? "access flag" : "no");
    devinfo_prop(d, "pan", "%s", field(mmfr1, 20) ? "yes" : "no");
    devinfo_prop(d, "vhe", "%s", field(mmfr1, 8) ? "yes" : "no");
    devinfo_prop(d, "clock_rate", "%lu Hz (generic timer)", (unsigned long)timer_clock_hz());
    char list[512];
    features(list, sizeof list);
    devinfo_prop(d, "features", "%s", list);
    devinfo_prop(d, "id_aa64isar0", "0x%016lx", (unsigned long)READ_SYSREG(id_aa64isar0_el1));
    devinfo_prop(d, "id_aa64isar1", "0x%016lx", (unsigned long)READ_SYSREG(id_aa64isar1_el1));
    devinfo_prop(d, "id_aa64pfr0", "0x%016lx", (unsigned long)pfr0);
    devinfo_prop(d, "id_aa64pfr1", "0x%016lx", (unsigned long)READ_SYSREG(id_aa64pfr1_el1));
    devinfo_prop(d, "id_aa64mmfr0", "0x%016lx", (unsigned long)mmfr0);
    devinfo_prop(d, "id_aa64mmfr1", "0x%016lx", (unsigned long)mmfr1);
    devinfo_prop(d, "id_aa64mmfr2", "0x%016lx", (unsigned long)mmfr2);
    devinfo_prop(d, "ctr", "0x%016lx", (unsigned long)READ_SYSREG(ctr_el0));
    describe_caches(d);
    for (unsigned i = 0; i < smp_cpu_count(); i++) {
        struct cpu *c = cpu_by_id(i);
        char path[16];
        ksnprintf(path, sizeof path, "cpu/%u", i);
        midr_name(c->arch.midr, name, sizeof name);
        devinfo_node(d, path, "CPU %u", i);
        devinfo_prop(d, "cpu_id", "%u", i);
        devinfo_prop(d, "model_name", "%s", c->arch.midr ? name : "not started");
        devinfo_prop(d, "mpidr", "0x%lx (affinity %lu.%lu.%lu.%lu)", (unsigned long)c->arch.mpidr,
                     (unsigned long)(c->arch.mpidr >> 32) & 0xff, (unsigned long)(c->arch.mpidr >> 16) & 0xff,
                     (unsigned long)(c->arch.mpidr >> 8) & 0xff, (unsigned long)c->arch.mpidr & 0xff);
        devinfo_prop(d, "midr", "0x%08lx", (unsigned long)c->arch.midr);
        devinfo_prop(d, "boot_cpu", "%s", i == 0 ? "yes" : "no");
        devinfo_prop(d, "started", "%s", __atomic_load_n(&c->started, __ATOMIC_ACQUIRE) ? "yes" : "no");
    }
}

static void describe_platform(struct devinfo *d)
{
    devinfo_node(d, "platform", "%s", devtree.model[0] ? devtree.model : "Arm virtual machine");
    devinfo_prop(d, "machine", "%s", devtree.model[0] ? devtree.model : "unknown");
    devinfo_prop(d, "description_source", "%s",
                 devtree.from_acpi ? "ACPI tables" : bootinfo.dtb ? "device tree" : "fixed addresses of QEMU virt");
    gic_describe(d);
    if (devtree.ecam) {
        devinfo_prop(d, "pcie_ecam", "0x%lx, buses %u to %u", (unsigned long)devtree.ecam, devtree.bus_start,
                     devtree.bus_end);
        if (devtree.gic_version == 3)
            devinfo_prop(d, "msi_mapping", "requester IDs 0x%x to 0x%x as device IDs from 0x%x", devtree.msi_rid_base,
                         devtree.msi_rid_base + devtree.msi_length - 1, devtree.msi_base);
    }
    devinfo_prop(d, "serial_console", "PL011 at 0x9000000");
    devinfo_prop(d, "real_time_clock", "PL031 at 0x%lx", (unsigned long)devtree.rtc);
    uint32_t v = platform_psci_version();
    devinfo_prop(d, "psci", "%u.%u through %s", v >> 16, v & 0xffff, devtree.psci_smc ? "smc" : "hvc");
    devinfo_prop(d, "power_off", "PSCI SYSTEM_OFF");
    devinfo_prop(d, "reboot", "PSCI SYSTEM_RESET");
    if (!devtree.from_acpi && (devtree.model[0] || devtree.compatible[0])) {
        devinfo_node(d, "firmware/devicetree", "Device tree");
        devinfo_prop(d, "model", "%s", devtree.model);
        devinfo_prop(d, "compatible", "%s", devtree.compatible);
    }
}

void arch_describe(struct devinfo *d)
{
    describe_cpus(d);
    describe_platform(d);
}
