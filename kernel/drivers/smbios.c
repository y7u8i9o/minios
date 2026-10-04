/* The SMBIOS tables (DMTF DSP0134), which name the firmware, the system,
 * the baseboard, the chassis, the processor sockets and the memory
 * modules. smbios_init copies the structure table from the firmware once.
 * smbios_describe writes it to /dev/devices (docs/design/sysinfo.md). */
#define KLOG_SUBSYS "smbios"
#include <drivers/devinfo.h>
#include <boot.h>
#include <mm/slab.h>
#include <mm/vmm.h>
#include <lib/endian.h>
#include <lib/guid.h>
#include <lib/printf.h>
#include <lib/string.h>
#include <klog.h>

#define MAX_TABLE (64 * 1024)

/* The copy of the structure table. Written once by smbios_init before
 * /dev/devices can be opened, read only afterwards. */
static uint8_t *table;
static size_t table_len;
static unsigned version_major, version_minor;

void smbios_init(void)
{
    uint8_t ep[32];
    uint64_t addr = 0;
    size_t len = 0;
    if (bootinfo.smbios64_phys && vmm_copy_from_phys(ep, bootinfo.smbios64_phys, 24) && memcmp(ep, "_SM3_", 5) == 0) {
        version_major = ep[7];
        version_minor = ep[8];
        len = get_le32(ep + 12);
        addr = get_le64(ep + 16);
    } else if (bootinfo.smbios32_phys && vmm_copy_from_phys(ep, bootinfo.smbios32_phys, 31) && memcmp(ep, "_SM_", 4) == 0) {
        version_major = ep[6];
        version_minor = ep[7];
        len = get_le16(ep + 0x16);
        addr = get_le32(ep + 0x18);
    } else {
        klog_info("no SMBIOS tables");
        return;
    }
    len = MIN(len, (size_t)MAX_TABLE);
    table = kmalloc(len);
    if (!table || !vmm_copy_from_phys(table, addr, len)) {
        kfree(table);
        table = NULL;
        klog_warn("the SMBIOS table at %lx cannot be read", (unsigned long)addr);
        return;
    }
    table_len = len;
    klog_info("SMBIOS %u.%u, %zu byte table", version_major, version_minor, table_len);
}

/* A structure: its formatted area and the strings after it. */
struct sm {
    const uint8_t *p;           /* the formatted area */
    unsigned len;               /* its length */
    const char *strings;        /* the first string */
    const uint8_t *end;         /* the first byte after the string set */
};

/* The next structure from *off, or false at the end of the table. */
static bool next_structure(size_t *off, struct sm *s)
{
    if (*off + 4 > table_len)
        return false;
    const uint8_t *p = table + *off;
    unsigned len = p[1];
    if (len < 4 || *off + len > table_len)
        return false;
    /* The string set ends with two zero bytes. */
    size_t i = *off + len;
    while (i + 1 < table_len && !(table[i] == 0 && table[i + 1] == 0))
        i++;
    if (i + 1 >= table_len)
        return false;
    s->p = p;
    s->len = len;
    s->strings = (const char *)(table + *off + len);
    s->end = table + i + 2;
    *off = i + 2;
    return true;
}

/* String number index (from 1) of a structure, or "" for 0 or an index
 * beyond its strings. */
static const char *sm_string(const struct sm *s, unsigned off)
{
    if (off >= s->len || s->p[off] == 0)
        return "";
    unsigned index = s->p[off];
    const char *str = s->strings;
    for (unsigned i = 1; i < index && (const uint8_t *)str < s->end; i++)
        str += strlen(str) + 1;
    return (const uint8_t *)str < s->end ? str : "";
}

static uint8_t byte_at(const struct sm *s, unsigned off)
{
    return off < s->len ? s->p[off] : 0;
}

static uint16_t word_at(const struct sm *s, unsigned off)
{
    return off + 1 < s->len ? get_le16(s->p + off) : 0;
}

static uint32_t dword_at(const struct sm *s, unsigned off)
{
    return off + 3 < s->len ? get_le32(s->p + off) : 0;
}

/* A string property, left out when the firmware gave none. */
static void prop_string(struct devinfo *d, const char *key, const struct sm *s, unsigned off)
{
    const char *v = sm_string(s, off);
    if (v[0])
        devinfo_prop(d, key, "%s", v);
}

static const char *chassis_type(unsigned t)
{
    static const char *const names[] = {
        [1] = "other", [2] = "unknown", [3] = "desktop", [4] = "low profile desktop", [5] = "pizza box",
        [6] = "mini tower", [7] = "tower", [8] = "portable", [9] = "laptop", [10] = "notebook",
        [11] = "handheld", [12] = "docking station", [13] = "all in one", [14] = "sub notebook",
        [15] = "space-saving", [16] = "lunch box", [17] = "main server chassis", [18] = "expansion chassis",
        [19] = "sub chassis", [20] = "bus expansion chassis", [21] = "peripheral chassis",
        [22] = "RAID chassis", [23] = "rack mount chassis", [24] = "sealed-case PC", [25] = "multi-system chassis",
        [26] = "compact PCI", [27] = "advanced TCA", [28] = "blade", [29] = "blade enclosure", [30] = "tablet",
        [31] = "convertible", [32] = "detachable", [33] = "IoT gateway", [34] = "embedded PC",
        [35] = "mini PC", [36] = "stick PC",
    };
    return t < sizeof names / sizeof names[0] && names[t] ? names[t] : "unknown";
}

static const char *memory_type(unsigned t)
{
    static const char *const names[] = {
        [1] = "other", [2] = "unknown", [3] = "DRAM", [4] = "EDRAM", [5] = "VRAM", [6] = "SRAM", [7] = "RAM",
        [8] = "ROM", [9] = "flash", [10] = "EEPROM", [11] = "FEPROM", [12] = "EPROM", [13] = "CDRAM",
        [14] = "3DRAM", [15] = "SDRAM", [16] = "SGRAM", [17] = "RDRAM", [18] = "DDR", [19] = "DDR2",
        [20] = "DDR2 FB-DIMM", [24] = "DDR3", [25] = "FBD2", [26] = "DDR4", [27] = "LPDDR", [28] = "LPDDR2",
        [29] = "LPDDR3", [30] = "LPDDR4", [31] = "logical non-volatile device", [32] = "HBM", [33] = "HBM2",
        [34] = "DDR5", [35] = "LPDDR5", [36] = "HBM3",
    };
    return t < sizeof names / sizeof names[0] && names[t] ? names[t] : "unknown";
}

static const char *form_factor(unsigned t)
{
    static const char *const names[] = {
        [1] = "other", [2] = "unknown", [3] = "SIMM", [4] = "SIP", [5] = "chip", [6] = "DIP", [7] = "ZIP",
        [8] = "proprietary card", [9] = "DIMM", [10] = "TSOP", [11] = "row of chips", [12] = "RIMM",
        [13] = "SODIMM", [14] = "SRIMM", [15] = "FB-DIMM", [16] = "die", [17] = "CAMM",
    };
    return t < sizeof names / sizeof names[0] && names[t] ? names[t] : "unknown";
}

static void describe_bios(struct devinfo *d, const struct sm *s)
{
    devinfo_node(d, "firmware/smbios/bios", "Firmware: %s %s", sm_string(s, 4), sm_string(s, 5));
    prop_string(d, "vendor", s, 4);
    prop_string(d, "version", s, 5);
    prop_string(d, "release_date", s, 8);
    uint8_t rom = byte_at(s, 9);
    if (rom && rom != 0xff)
        devinfo_size(d, "rom_size", (uint64_t)(rom + 1) * 64 * 1024);
    if (s->len >= 0x16 && byte_at(s, 0x14) != 0xff)
        devinfo_prop(d, "release", "%u.%u", byte_at(s, 0x14), byte_at(s, 0x15));
    uint64_t ch = (uint64_t)dword_at(s, 0x0a) | (uint64_t)dword_at(s, 0x0e) << 32;
    if (s->len >= 0x13)
        devinfo_prop(d, "uefi_supported", "%s", (byte_at(s, 0x13) & (1u << 3)) ? "yes" : "no");
    devinfo_prop(d, "characteristics", "0x%016lx", (unsigned long)ch);
}

static void describe_system_info(struct devinfo *d, const struct sm *s)
{
    devinfo_node(d, "firmware/smbios/system", "System: %s %s", sm_string(s, 4), sm_string(s, 5));
    prop_string(d, "manufacturer", s, 4);
    prop_string(d, "product", s, 5);
    prop_string(d, "version", s, 6);
    prop_string(d, "serial", s, 7);
    if (s->len >= 0x19) {
        /* The first three fields of the UUID are little endian since
         * SMBIOS 2.6, the byte order of GPT GUIDs. */
        char uuid[GUID_STR];
        guid_format(s->p + 8, uuid);
        devinfo_prop(d, "uuid", "%s", uuid);
    }
    prop_string(d, "sku", s, 0x19);
    prop_string(d, "family", s, 0x1a);
}

static void describe_baseboard(struct devinfo *d, const struct sm *s)
{
    devinfo_node(d, "firmware/smbios/baseboard", "Baseboard: %s %s", sm_string(s, 4), sm_string(s, 5));
    prop_string(d, "manufacturer", s, 4);
    prop_string(d, "product", s, 5);
    prop_string(d, "version", s, 6);
    prop_string(d, "serial", s, 7);
    prop_string(d, "asset_tag", s, 8);
}

static void describe_chassis(struct devinfo *d, const struct sm *s)
{
    devinfo_node(d, "firmware/smbios/chassis", "Chassis: %s", chassis_type(byte_at(s, 5) & 0x7f));
    prop_string(d, "manufacturer", s, 4);
    devinfo_prop(d, "chassis_type", "%s", chassis_type(byte_at(s, 5) & 0x7f));
    prop_string(d, "version", s, 6);
    prop_string(d, "serial", s, 7);
    prop_string(d, "asset_tag", s, 8);
}

static void describe_processor(struct devinfo *d, const struct sm *s, unsigned n)
{
    char path[48];
    ksnprintf(path, sizeof path, "firmware/smbios/processor%u", n);
    devinfo_node(d, path, "Processor socket %s", sm_string(s, 4));
    prop_string(d, "socket", s, 4);
    prop_string(d, "manufacturer", s, 7);
    prop_string(d, "version", s, 0x10);
    uint16_t ext = word_at(s, 0x12), max = word_at(s, 0x14), cur = word_at(s, 0x16);
    if (ext)
        devinfo_prop(d, "external_clock", "%u MHz", ext);
    if (max)
        devinfo_prop(d, "max_speed", "%u MHz", max);
    if (cur)
        devinfo_prop(d, "current_speed", "%u MHz", cur);
    devinfo_prop(d, "populated", "%s", (byte_at(s, 0x18) & 0x40) ? "yes" : "no");
    if (s->len >= 0x26) {
        devinfo_prop(d, "cores", "%u", byte_at(s, 0x23));
        devinfo_prop(d, "cores_enabled", "%u", byte_at(s, 0x24));
        devinfo_prop(d, "threads", "%u", byte_at(s, 0x25));
    }
    prop_string(d, "serial", s, 0x20);
    prop_string(d, "part_number", s, 0x22);
}

static void describe_memory_device(struct devinfo *d, const struct sm *s, unsigned n)
{
    uint16_t size = word_at(s, 0x0c);
    char path[48];
    ksnprintf(path, sizeof path, "firmware/smbios/memory%u", n);
    devinfo_node(d, path, "Memory module %s", sm_string(s, 0x10));
    prop_string(d, "locator", s, 0x10);
    prop_string(d, "bank", s, 0x11);
    if (size == 0)
        devinfo_prop(d, "installed", "no");
    else if (size == 0xffff)
        devinfo_prop(d, "module_size", "unknown");
    else if (size == 0x7fff)
        devinfo_size(d, "module_size", (uint64_t)(dword_at(s, 0x1c) & 0x7fffffff) << 20);
    else
        devinfo_size(d, "module_size", (size & 0x8000) ? (uint64_t)(size & 0x7fff) << 10 : (uint64_t)size << 20);
    devinfo_prop(d, "form_factor", "%s", form_factor(byte_at(s, 0x0e)));
    devinfo_prop(d, "memory_type", "%s", memory_type(byte_at(s, 0x12)));
    if (word_at(s, 0x15))
        devinfo_prop(d, "speed", "%u MT/s", word_at(s, 0x15));
    if (word_at(s, 0x20))
        devinfo_prop(d, "configured_speed", "%u MT/s", word_at(s, 0x20));
    prop_string(d, "manufacturer", s, 0x17);
    prop_string(d, "serial", s, 0x18);
    prop_string(d, "part_number", s, 0x1a);
}

void smbios_describe(struct devinfo *d)
{
    if (!table)
        return;
    unsigned count = 0, processors = 0, modules = 0;
    size_t off = 0;
    struct sm s;
    while (next_structure(&off, &s) && s.p[0] != 127)
        count++;
    devinfo_node(d, "firmware/smbios", "SMBIOS %u.%u", version_major, version_minor);
    devinfo_prop(d, "version", "%u.%u", version_major, version_minor);
    devinfo_size(d, "table_size", table_len);
    devinfo_prop(d, "structures", "%u", count);
    off = 0;
    while (next_structure(&off, &s) && s.p[0] != 127) {
        switch (s.p[0]) {
        case 0:  describe_bios(d, &s); break;
        case 1:  describe_system_info(d, &s); break;
        case 2:  describe_baseboard(d, &s); break;
        case 3:  describe_chassis(d, &s); break;
        case 4:  describe_processor(d, &s, processors++); break;
        case 17: describe_memory_device(d, &s, modules++); break;
        default: break;
        }
    }
}
