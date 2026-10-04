/* /dev/devices (docs/design/sysinfo.md): the format of the text and the
 * nodes that every machine of the boot tests has. The case attaches a USB
 * keyboard and tablet, so the USB nodes are checked too. The text is
 * written to the serial log for inspection. */
#include <tests/ktest.h>
#include <fs/vfs.h>
#include <mm/slab.h>
#include <lib/printf.h>
#include <lib/string.h>
#include <console.h>
#include <errno.h>

#define MAX_TEXT (1024 * 1024)

static char *read_all(size_t *len)
{
    struct file *f;
    ktest_assert(vfs_open("/dev/devices", O_RDONLY, 0, &f) == 0, "cannot open /dev/devices");
    char *buf = kmalloc(MAX_TEXT);
    ktest_assert(buf, "no memory");
    size_t n = 0;
    for (;;) {
        long r = file_read(f, buf + n, MAX_TEXT - 1 - n);
        ktest_assert(r >= 0, "read returned %ld", r);
        if (r == 0)
            break;
        n += (size_t)r;
    }
    file_put(f);
    buf[n] = '\0';
    *len = n;
    return buf;
}

/* Whether a node line "@path<TAB>" is present. */
static bool has_node(const char *text, const char *path)
{
    char needle[96];
    ksnprintf(needle, sizeof needle, "\n@%s\t", path);
    return text[0] == '@' && strncmp(text + 1, path, strlen(path)) == 0 ? text[1 + strlen(path)] == '\t'
                                                                         : strstr(text, needle) != NULL;
}

static void test_devices(void)
{
    size_t len;
    char *text = read_all(&len);
    ktest_assert(len > 0 && text[0] == '@', "the text does not start with a node");
    ktest_assert(text[len - 1] == '\n', "the text does not end with a newline");
    unsigned nodes = 0, props = 0;
    for (char *line = text; *line;) {
        char *end = strchr(line, '\n');
        ktest_assert(end, "a line without a newline");
        char *tab = memchr(line, '\t', (size_t)(end - line));
        ktest_assert(tab && tab > line, "a line without a key: %.40s", line);
        if (line[0] == '@')
            nodes++;
        else
            props++;
        line = end + 1;
    }
    static const char *const required[] = {
        "system", "firmware", "cpu", "cpu/0", "platform", "memory", "memory/map", "memory/map/0", "pci",
        "pci/00:00.0", "usb", "usb/xhci0", "input", "storage", "storage/vda", "storage/filesystems", "display",
        "network", "network/lo",
    };
    for (size_t i = 0; i < sizeof required / sizeof required[0]; i++)
        ktest_assert(has_node(text, required[i]), "no node %s", required[i]);
    ktest_assert(strstr(text, "\nproduct\tQEMU USB Keyboard\n"), "no USB keyboard device node");
    ktest_assert(strstr(text, "\ndriver\tusb-hid\n"), "no interface bound to usb-hid");
    ktest_assert(strstr(text, "\ndriver\txhci\n"), "no PCI function bound to xhci");
    ktest_assert(strstr(text, "\ndriver\tvirtio-blk\n"), "no PCI function bound to virtio-blk");
    /* The root is the mfs of vda, and the disk node names its mount point. */
    ktest_assert(strstr(text, "\nfilesystem\tmfs\nmount_point\t/\n"), "the root mount is not on its disk");
    /* A machine with ACPI tables lists them, with the MADT among them. */
    if (strstr(text, "\nacpi\tyes\n")) {
        ktest_assert(has_node(text, "firmware/acpi"), "ACPI tables, but no node firmware/acpi");
        ktest_assert(strstr(text, "\nsignature\tAPIC\n"), "no MADT in the ACPI table list");
    }
    kprintf("devices: begin\n");
    console_write_user(text, len);
    kprintf("devices: end\n");
    kprintf("devices: %u nodes, %u properties, %zu bytes\n", nodes, props, len);
    kfree(text);
}
KTEST_DEFINE("devices", test_devices);
