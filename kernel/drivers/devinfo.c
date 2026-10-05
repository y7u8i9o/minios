/* /dev/devices (docs/design/sysinfo.md): the text description of the
 * machine. The file is made when it is opened. Every subsystem writes its
 * part through devinfo_node and devinfo_prop. This file writes the system
 * and the memory and collects the other parts. */
#define KLOG_SUBSYS "devinfo"
#include <drivers/devinfo.h>
#include <drivers/timer.h>
#include <drivers/rtc.h>
#include <arch/platform.h>
#include <arch/smp.h>
#include <boot.h>
#include <fs/devfs.h>
#include <fs/vfs.h>
#include <mm/pmm.h>
#include <mm/swap.h>
#include <mm/slab.h>
#include <lib/guid.h>
#include <lib/printf.h>
#include <lib/string.h>
#include <limine.h>
#include <stdarg.h>
#include <errno.h>

/* The text being written. When the buffer is full, overflow is set, the
 * rest is dropped and the open retries with a buffer twice the size. */
struct devinfo {
    char *buf;
    size_t len, size;
    bool overflow;
};

static void put(struct devinfo *d, const char *s, size_t n)
{
    if (d->overflow || d->len + n > d->size) {
        d->overflow = true;
        return;
    }
    memcpy(d->buf + d->len, s, n);
    d->len += n;
}

/* Append text with every tab and newline replaced by a space. */
static void put_clean(struct devinfo *d, const char *s)
{
    size_t start = d->len;
    put(d, s, strlen(s));
    if (d->overflow)
        return;
    for (size_t i = start; i < d->len; i++)
        if (d->buf[i] == '\t' || d->buf[i] == '\n')
            d->buf[i] = ' ';
}

void devinfo_node(struct devinfo *d, const char *path, const char *title_fmt, ...)
{
    char title[160];
    va_list ap;
    va_start(ap, title_fmt);
    kvsnprintf(title, sizeof title, title_fmt, ap);
    va_end(ap);
    put(d, "@", 1);
    put_clean(d, path);
    put(d, "\t", 1);
    put_clean(d, title);
    put(d, "\n", 1);
}

void devinfo_prop(struct devinfo *d, const char *key, const char *fmt, ...)
{
    char value[256];
    va_list ap;
    va_start(ap, fmt);
    kvsnprintf(value, sizeof value, fmt, ap);
    va_end(ap);
    put_clean(d, key);
    put(d, "\t", 1);
    put_clean(d, value);
    put(d, "\n", 1);
}

void devinfo_append(char *buf, size_t size, const char *sep, const char *word)
{
    if (buf[0])
        strlcat(buf, sep, size);
    strlcat(buf, word, size);
}

void devinfo_format_size(char *buf, size_t size, uint64_t bytes)
{
    static const char *const units[] = { "KiB", "MiB", "GiB", "TiB" };
    if (bytes < 1024) {
        ksnprintf(buf, size, "%lu bytes", (unsigned long)bytes);
        return;
    }
    unsigned u = 0;
    uint64_t scaled = bytes / 1024, rest = bytes % 1024;
    while (scaled >= 1024 && u < 3) {
        rest = scaled % 1024;
        scaled /= 1024;
        u++;
    }
    /* One decimal, rounded down, without floating point. A whole number
     * of units has no decimal. */
    if (rest)
        ksnprintf(buf, size, "%lu.%lu %s", (unsigned long)scaled, (unsigned long)(rest * 10 / 1024), units[u]);
    else
        ksnprintf(buf, size, "%lu %s", (unsigned long)scaled, units[u]);
}

void devinfo_size(struct devinfo *d, const char *key, uint64_t bytes)
{
    char rounded[32];
    devinfo_format_size(rounded, sizeof rounded, bytes);
    if (bytes < 1024)
        devinfo_prop(d, key, "%s", rounded);
    else
        devinfo_prop(d, key, "%lu bytes (%s)", (unsigned long)bytes, rounded);
}

static const char *architecture(void)
{
#if defined(__x86_64__)
    return "x86_64";
#elif defined(__aarch64__)
    return "aarch64";
#else
    return "unknown";
#endif
}

static const char *firmware_name(int type)
{
    switch (type) {
    case LIMINE_FIRMWARE_TYPE_X86BIOS: return "BIOS";
    case LIMINE_FIRMWARE_TYPE_EFI32:   return "UEFI, 32 bit";
    case LIMINE_FIRMWARE_TYPE_EFI64:   return "UEFI, 64 bit";
    case LIMINE_FIRMWARE_TYPE_SBI:     return "SBI";
    default:                           return "unknown";
    }
}

/* A GUID property in the string form of lib/guid.h. */
static void prop_guid(struct devinfo *d, const char *key, const uint8_t g[16])
{
    char s[GUID_STR];
    guid_format(g, s);
    devinfo_prop(d, key, "%s", s);
}

static void describe_system(struct devinfo *d)
{
    devinfo_node(d, "system", "minios %s", kernel_release);
    devinfo_prop(d, "os_name", "minios");
    devinfo_prop(d, "os_release", "%s", kernel_release);
    devinfo_prop(d, "kernel_build", "%u", kernel_build_number);
    devinfo_prop(d, "kernel_version", "%s", kernel_version);
    devinfo_prop(d, "architecture", "%s", architecture());
    devinfo_prop(d, "cpus", "%u", smp_cpu_count());
    uint64_t ms = timer_ms();
    devinfo_prop(d, "uptime", "%lu s (%lu d %02lu:%02lu:%02lu)", (unsigned long)(ms / 1000),
                 (unsigned long)(ms / 86400000), (unsigned long)(ms / 3600000 % 24),
                 (unsigned long)(ms / 60000 % 60), (unsigned long)(ms / 1000 % 60));
    struct rtc_date t;
    platform_rtc_read(&t);
    devinfo_prop(d, "rtc", "%04d-%02d-%02d %02d:%02d:%02d UTC", t.year, t.month, t.day, t.hour, t.minute, t.second);
    devinfo_prop(d, "tick_rate", "%u Hz", (unsigned)TIMER_HZ);
    devinfo_prop(d, "page_size", "%u bytes", (unsigned)PAGE_SIZE);
    devinfo_prop(d, "bootloader", "%s %s", bootinfo.bootloader_name[0] ? bootinfo.bootloader_name : "unknown",
                 bootinfo.bootloader_version);
    devinfo_prop(d, "boot_protocol", "Limine, base revision 4");
    devinfo_prop(d, "command_line", "%s", bootinfo.cmdline);
    if (!guid_is_zero(bootinfo.boot_disk_guid))
        prop_guid(d, "boot_disk_guid", bootinfo.boot_disk_guid);
    if (!guid_is_zero(bootinfo.boot_part_guid))
        prop_guid(d, "boot_partition_guid", bootinfo.boot_part_guid);
    devinfo_prop(d, "kernel_phys_base", "0x%lx", (unsigned long)bootinfo.kernel_phys_base);
    devinfo_prop(d, "kernel_virt_base", "0x%lx", (unsigned long)bootinfo.kernel_virt_base);
    devinfo_prop(d, "direct_map_base", "0x%lx", (unsigned long)bootinfo.hhdm_offset);
    if (bootinfo.initrd)
        devinfo_size(d, "initrd_size", bootinfo.initrd_size);
    devinfo_prop(d, "config", "tests %d, lockdebug %d, lockstat %d, slabdebug %d, log level %d", CONFIG_TESTS,
                 CONFIG_LOCKDEBUG, CONFIG_LOCKSTAT, CONFIG_SLABDEBUG, CONFIG_LOG_LEVEL);
}

static const char *memmap_type(uint64_t type)
{
    switch (type) {
    case LIMINE_MEMMAP_USABLE:                 return "usable";
    case LIMINE_MEMMAP_RESERVED:               return "reserved";
    case LIMINE_MEMMAP_ACPI_RECLAIMABLE:       return "acpi reclaimable";
    case LIMINE_MEMMAP_ACPI_NVS:               return "acpi nvs";
    case LIMINE_MEMMAP_BAD_MEMORY:             return "bad";
    case LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE: return "bootloader reclaimable";
    case LIMINE_MEMMAP_EXECUTABLE_AND_MODULES: return "kernel and modules";
    case LIMINE_MEMMAP_FRAMEBUFFER:            return "framebuffer";
    case LIMINE_MEMMAP_RESERVED_MAPPED:        return "reserved, mapped";
    default:                                   return "unknown";
    }
}

static void describe_memory(struct devinfo *d)
{
    struct pmm_stats st;
    struct swap_stats sw;
    pmm_get_stats(&st);
    swap_get_stats(&sw);
    uint64_t ram = 0;
    for (size_t i = 0; i < bootinfo.memmap_count; i++) {
        uint64_t t = bootinfo.memmap[i].type;
        if (t == LIMINE_MEMMAP_USABLE || t == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE ||
            t == LIMINE_MEMMAP_EXECUTABLE_AND_MODULES || t == LIMINE_MEMMAP_ACPI_RECLAIMABLE)
            ram += bootinfo.memmap[i].length;
    }
    devinfo_node(d, "memory", "Memory");
    devinfo_size(d, "ram", ram);
    devinfo_size(d, "managed", st.total_pages * PAGE_SIZE);
    devinfo_size(d, "free", st.free_pages * PAGE_SIZE);
    devinfo_size(d, "used", (st.total_pages - st.free_pages) * PAGE_SIZE);
    devinfo_size(d, "swap_total", sw.total_slots * PAGE_SIZE);
    devinfo_size(d, "swap_free", sw.free_slots * PAGE_SIZE);
    devinfo_prop(d, "memory_map_entries", "%zu", bootinfo.memmap_count);
    devinfo_node(d, "memory/map", "Memory map");
    devinfo_prop(d, "entries", "%zu", bootinfo.memmap_count);
    for (size_t i = 0; i < bootinfo.memmap_count; i++) {
        const struct limine_memmap_entry *e = &bootinfo.memmap[i];
        char path[32];
        ksnprintf(path, sizeof path, "memory/map/%zu", i);
        devinfo_node(d, path, "%016lx-%016lx %s", (unsigned long)e->base,
                     (unsigned long)(e->base + e->length - 1), memmap_type(e->type));
        devinfo_prop(d, "type", "%s", memmap_type(e->type));
        devinfo_prop(d, "base", "0x%lx", (unsigned long)e->base);
        devinfo_prop(d, "end", "0x%lx", (unsigned long)(e->base + e->length - 1));
        devinfo_size(d, "length", e->length);
    }
}

static void describe_firmware(struct devinfo *d)
{
    devinfo_node(d, "firmware", "Firmware");
    devinfo_prop(d, "firmware_type", "%s", firmware_name(bootinfo.firmware_type));
    devinfo_prop(d, "acpi", "%s", bootinfo.rsdp_phys ? "yes" : "no");
    if (bootinfo.rsdp_phys)
        devinfo_prop(d, "rsdp", "0x%lx", (unsigned long)bootinfo.rsdp_phys);
    devinfo_prop(d, "smbios", "%s", bootinfo.smbios32_phys || bootinfo.smbios64_phys ? "yes" : "no");
    devinfo_prop(d, "device_tree", "%s", bootinfo.dtb ? "yes" : "no");
    acpi_tables_describe(d);
}

static void describe_all(struct devinfo *d)
{
    describe_system(d);
    describe_firmware(d);
    arch_describe(d);
    smbios_describe(d);
    describe_memory(d);
    pci_describe(d);
    usb_describe(d);
    nvme_describe(d);
    ahci_describe(d);
    input_describe(d);
    block_describe(d);
    fbdev_describe(d);
    pcm_describe(d);
    netif_describe(d);
}

/* The text of one open file. */
struct snapshot {
    char *text;
    size_t len;
};

static int devices_open(struct inode *ino, struct file *f)
{
    struct snapshot *s = kzalloc(sizeof *s);
    if (!s)
        return -ENOMEM;
    for (size_t size = 64 * 1024; size <= 4 * 1024 * 1024; size *= 2) {
        struct devinfo d = { .buf = kmalloc(size), .size = size };
        if (!d.buf)
            break;
        describe_all(&d);
        if (!d.overflow) {
            s->text = d.buf;
            s->len = d.len;
            f->priv = s;
            return 0;
        }
        kfree(d.buf);
    }
    kfree(s);
    return -ENOMEM;
}

static long devices_read(struct file *f, char *buf, size_t n, uint64_t *pos)
{
    struct snapshot *s = f->priv;
    if (*pos >= s->len)
        return 0;
    n = MIN(n, s->len - (size_t)*pos);
    memcpy(buf, s->text + *pos, n);
    *pos += n;
    return (long)n;
}

static void devices_release(struct file *f)
{
    struct snapshot *s = f->priv;
    if (s) {
        kfree(s->text);
        kfree(s);
    }
}

static const struct file_ops devices_fops = {
    .open = devices_open, .read = devices_read, .release = devices_release,
};

void devinfo_init(void)
{
    smbios_init();
    acpi_tables_init();
    devfs_register("devices", S_IFCHR | 0444, &devices_fops, NULL, 0);
}
