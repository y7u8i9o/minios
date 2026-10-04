/* sysinfo shows the machine and its devices as /dev/devices describes them
 * (docs/design/sysinfo.md).
 *
 * The tree on the left lists the categories and the nodes below them. The
 * table on the right lists the properties of the selected node. The
 * property names are translated through the table of known keys below.
 * The values are technical data and are shown as the kernel writes them.
 * View > Refresh (F5) reads /dev/devices again and selects the same node.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <gui/app.h>
#include <gui/model.h>
#include <gui/i18n.h>

#define MAX_TEXT (2 * 1024 * 1024)

struct prop {
    const char *key, *value;
};

struct node {
    const char *path, *title;
    int parent;                     /* -1 for a category */
    int *children;
    int nchildren;
    int first_prop, nprops;
};

/* The parsed text. Every string points into text. */
static char *text;
static struct node *nodes;
static int nnodes;
static struct prop *props;
static int nprops;
static int *roots;
static int nroots;

static struct app *app;
static struct widget *win, *tree, *table, *status;
static int selected = -1;

/* The categories of /dev/devices, in the order of the text. */
static const struct { const char *path, *label; } categories[] = {
    { "system", N_("System") },        { "firmware", N_("Firmware") }, { "cpu", N_("Processors") },
    { "platform", N_("Platform") },    { "memory", N_("Memory") },     { "pci", N_("PCI") },
    { "usb", N_("USB") },              { "input", N_("Input devices") }, { "storage", N_("Storage") },
    { "display", N_("Display") },      { "audio", N_("Audio") },       { "network", N_("Network") },
};

/* The labels of the property keys of /dev/devices. */
static const struct { const char *key, *label; } labels[] = {
    { "acpi", N_("ACPI") },
    { "acquired", N_("Acquired") },
    { "address", N_("Address") },
    { "alternate_setting", N_("Alternate setting") },
    { "amd_features", N_("AMD features") },
    { "apic_id", N_("APIC ID") },
    { "architecture", N_("Architecture") },
    { "asid_bits", N_("ASID bits") },
    { "asset_tag", N_("Asset tag") },
    { "bank", N_("Bank") },
    { "base", N_("Base address") },
    { "bits_per_pixel", N_("Bits per pixel") },
    { "boot_cpu", N_("Boot CPU") },
    { "boot_disk", N_("Boot disk") },
    { "boot_disk_guid", N_("Boot disk GUID") },
    { "boot_framebuffer", N_("Boot framebuffer") },
    { "boot_partition", N_("Boot partition") },
    { "boot_partition_guid", N_("Boot partition GUID") },
    { "boot_protocol", N_("Boot protocol") },
    { "bootloader", N_("Bootloader") },
    { "bus", N_("Bus") },
    { "buses", N_("Buses") },
    { "buttons", N_("Buttons") },
    { "cache_size", N_("Cache size") },
    { "cache_type", N_("Cache type") },
    { "capabilities", N_("Capabilities") },
    { "capture", N_("Capture") },
    { "characteristics", N_("Characteristics") },
    { "chassis_type", N_("Chassis type") },
    { "chipset", N_("Chipset") },
    { "class", N_("Class") },
    { "clock_rate", N_("Clock rate") },
    { "command", N_("Command register") },
    { "command_line", N_("Command line") },
    { "compatible", N_("Compatible") },
    { "config", N_("Kernel configuration") },
    { "configuration", N_("Configuration") },
    { "configurations", N_("Configurations") },
    { "configured_speed", N_("Configured speed") },
    { "connected", N_("Connected") },
    { "context_size", N_("Context size") },
    { "controllers", N_("Controllers") },
    { "cores", N_("Cores") },
    { "cores_enabled", N_("Cores enabled") },
    { "cpu_id", N_("CPU ID") },
    { "cpus", N_("CPUs") },
    { "creator", N_("Creator") },
    { "ctr", N_("CTR_EL0") },
    { "current_el", N_("Current exception level") },
    { "current_speed", N_("Current speed") },
    { "description_source", N_("Source of the description") },
    { "device", N_("Device") },
    { "device_class", N_("Device class") },
    { "device_node", N_("Device node") },
    { "device_release", N_("Device release") },
    { "device_tree", N_("Device tree") },
    { "devices", N_("Devices") },
    { "direct_map_base", N_("Direct map base") },
    { "disk_guid", N_("Disk GUID") },
    { "disks", N_("Disks") },
    { "driver", N_("Driver") },
    { "el0", N_("EL0 support") },
    { "el1", N_("EL1 support") },
    { "el2", N_("EL2 support") },
    { "el3", N_("EL3 support") },
    { "enabled", N_("Enabled") },
    { "end", N_("End address") },
    { "endpoints", N_("Endpoints") },
    { "entries", N_("Entries") },
    { "entry", N_("Table entry") },
    { "extended_features", N_("Extended features") },
    { "external_clock", N_("External clock") },
    { "family", N_("Family") },
    { "feature_words", N_("Feature words") },
    { "features", N_("Features") },
    { "firmware_type", N_("Firmware type") },
    { "first_sector", N_("First sector") },
    { "form_factor", N_("Form factor") },
    { "framebuffer", N_("Framebuffer") },
    { "framebuffer_size", N_("Framebuffer size") },
    { "free", N_("Free") },
    { "functions", N_("Functions") },
    { "gic_cpu_interface", N_("GIC CPU interface") },
    { "gic_distributor", N_("GIC distributor") },
    { "gic_redistributors", N_("GIC redistributors") },
    { "gicv2m", N_("GICv2m") },
    { "gpu_buffer", N_("GPU buffer") },
    { "gpu_pci_address", N_("GPU PCI address") },
    { "gpu_preferred_mode", N_("GPU preferred mode") },
    { "gpu_scanout", N_("GPU scanout") },
    { "grabbed", N_("Grabbed") },
    { "granules", N_("Page granules") },
    { "hardware_access_flag", N_("Hardware access flag update") },
    { "header_type", N_("Header type") },
    { "hypervisor", N_("Hypervisor") },
    { "id_aa64isar0", N_("ID_AA64ISAR0_EL1") },
    { "id_aa64isar1", N_("ID_AA64ISAR1_EL1") },
    { "id_aa64mmfr0", N_("ID_AA64MMFR0_EL1") },
    { "id_aa64mmfr1", N_("ID_AA64MMFR1_EL1") },
    { "id_aa64mmfr2", N_("ID_AA64MMFR2_EL1") },
    { "id_aa64pfr0", N_("ID_AA64PFR0_EL1") },
    { "id_aa64pfr1", N_("ID_AA64PFR1_EL1") },
    { "implementer", N_("Implementer") },
    { "index", N_("Index") },
    { "initrd_size", N_("Initial RAM disk size") },
    { "input_device", N_("Input device") },
    { "input_fields", N_("Input fields") },
    { "installed", N_("Installed") },
    { "interface", N_("Interface") },
    { "interface_class", N_("Interface class") },
    { "interface_version", N_("Interface version") },
    { "interfaces", N_("Interfaces") },
    { "interrupt", N_("Interrupt") },
    { "interrupt_controller", N_("Interrupt controller") },
    { "interrupt_pin", N_("Interrupt pin") },
    { "invariant_tsc", N_("Invariant TSC") },
    { "io_apic", N_("I/O APIC") },
    { "ipv4", N_("IPv4 address") },
    { "its", N_("ITS") },
    { "kernel_build", N_("Kernel build number") },
    { "kernel_phys_base", N_("Kernel physical base") },
    { "kernel_version", N_("Kernel version") },
    { "kernel_virt_base", N_("Kernel virtual base") },
    { "key_repeat", N_("Key repeat") },
    { "keyboard_controller", N_("Keyboard controller") },
    { "keys", N_("Keys") },
    { "length", N_("Length") },
    { "level", N_("Level") },
    { "line_size", N_("Line size") },
    { "link_state", N_("Link state") },
    { "link_type", N_("Link type") },
    { "local_apic", N_("Local APIC") },
    { "local_apic_mode", N_("Local APIC mode") },
    { "local_apic_timer", N_("Local APIC timer") },
    { "local_apic_version", N_("Local APIC version") },
    { "locator", N_("Locator") },
    { "logical_resolution", N_("Logical resolution") },
    { "lpis", N_("LPIs") },
    { "mac_address", N_("MAC address") },
    { "machine", N_("Machine") },
    { "managed", N_("Managed by the kernel") },
    { "manufacturer", N_("Manufacturer") },
    { "max_extended_leaf", N_("Maximum extended CPUID leaf") },
    { "max_leaf", N_("Maximum CPUID leaf") },
    { "max_packet_size_0", N_("Endpoint 0 packet size") },
    { "max_speed", N_("Maximum speed") },
    { "memory_map_entries", N_("Memory map entries") },
    { "memory_type", N_("Memory type") },
    { "midr", N_("MIDR_EL1") },
    { "model", N_("Model") },
    { "model_name", N_("Model name") },
    { "module_size", N_("Module size") },
    { "mpidr", N_("MPIDR_EL1") },
    { "msi", N_("MSI") },
    { "msi_mapping", N_("MSI mapping") },
    { "msi_x", N_("MSI-X") },
    { "mtu", N_("MTU") },
    { "name", N_("Name") },
    { "oem_id", N_("OEM ID") },
    { "oem_revision", N_("OEM revision") },
    { "oem_table_id", N_("OEM table ID") },
    { "open_readers", N_("Open readers") },
    { "opened", N_("Opened") },
    { "os_name", N_("Operating system") },
    { "os_release", N_("Release") },
    { "over_current", N_("Over-current") },
    { "page_size", N_("Page size") },
    { "pan", N_("Privileged access never") },
    { "part", N_("Part number") },
    { "part_number", N_("Part number") },
    { "partition_table", N_("Partition table") },
    { "partition_type", N_("Partition type") },
    { "pci_address", N_("PCI address") },
    { "pcie", N_("PCI Express") },
    { "pcie_ecam", N_("PCI Express configuration space") },
    { "pcie_link", N_("PCI Express link") },
    { "physical_address_bits", N_("Physical address bits") },
    { "pitch", N_("Pitch") },
    { "pixel_format", N_("Pixel format") },
    { "playback", N_("Playback") },
    { "populated", N_("Populated") },
    { "port", N_("Port") },
    { "ports", N_("Ports") },
    { "portsc", N_("PORTSC register") },
    { "power", N_("Power") },
    { "power_management", N_("Power management") },
    { "power_off", N_("Power off") },
    { "powered", N_("Powered") },
    { "product", N_("Product") },
    { "product_id", N_("Product ID") },
    { "protocol", N_("Protocol") },
    { "psci", N_("PSCI") },
    { "ram", N_("RAM") },
    { "real_time_clock", N_("Real time clock device") },
    { "reboot", N_("Reboot") },
    { "received", N_("Received") },
    { "relative_axes", N_("Relative axes") },
    { "release", N_("Firmware release") },
    { "release_date", N_("Release date") },
    { "report_ids", N_("Report IDs") },
    { "report_length", N_("Report length") },
    { "report_protocol", N_("Report protocol") },
    { "resolution", N_("Resolution") },
    { "revision", N_("Revision") },
    { "rom_size", N_("ROM size") },
    { "rsdp", N_("RSDP address") },
    { "rsdp_revision", N_("RSDP revision") },
    { "rtc", N_("Real time clock") },
    { "scratchpad_buffers", N_("Scratchpad buffers") },
    { "sector_size", N_("Sector size") },
    { "sectors", N_("Sectors") },
    { "sent", N_("Sent") },
    { "serial", N_("Serial number") },
    { "serial_console", N_("Serial console") },
    { "sets", N_("Sets") },
    { "shared_by", N_("Shared by") },
    { "signature", N_("Signature") },
    { "size", N_("Size") },
    { "sku", N_("SKU") },
    { "slot", N_("Slot") },
    { "slots", N_("Slots") },
    { "smbios", N_("SMBIOS") },
    { "socket", N_("Socket") },
    { "speed", N_("Speed") },
    { "started", N_("Started") },
    { "state", N_("State") },
    { "stepping", N_("Stepping") },
    { "structures", N_("Structures") },
    { "subsystem", N_("Subsystem") },
    { "supported_format", N_("Supported format") },
    { "swap_free", N_("Free swap") },
    { "swap_total", N_("Total swap") },
    { "table_size", N_("Table size") },
    { "tables", N_("Tables") },
    { "threads", N_("Threads") },
    { "tick_rate", N_("Scheduler tick rate") },
    { "type", N_("Type") },
    { "uefi_supported", N_("UEFI supported") },
    { "unique_guid", N_("Unique GUID") },
    { "uptime", N_("Uptime") },
    { "usb_version", N_("USB version") },
    { "used", N_("Used") },
    { "uuid", N_("UUID") },
    { "variant", N_("Variant") },
    { "vendor", N_("Vendor") },
    { "vendor_id", N_("Vendor ID") },
    { "version", N_("Version") },
    { "vhe", N_("Virtualization host extensions") },
    { "virtual_address_bits", N_("Virtual address bits") },
    { "ways", N_("Ways") },
};

/* The label of a key: a known key, one of the numbered keys BAR n,
 * endpoint and absolute axis, or the key with spaces for '_'. */
static const char *key_label(const char *key, char *buf, size_t size)
{
    for (size_t i = 0; i < sizeof labels / sizeof labels[0]; i++)
        if (strcmp(labels[i].key, key) == 0)
            return _(labels[i].label);
    if (strncmp(key, "bar", 3) == 0 && key[3] >= '0' && key[3] <= '5' && !key[4]) {
        snprintf(buf, size, _("BAR %c"), key[3]);
        return buf;
    }
    if (strncmp(key, "endpoint_", 9) == 0) {
        snprintf(buf, size, _("Endpoint 0x%s"), key + 9);
        return buf;
    }
    if (strncmp(key, "absolute_axis_", 14) == 0) {
        snprintf(buf, size, _("Absolute axis %s"), key + 14);
        return buf;
    }
    snprintf(buf, size, "%s", key);
    for (char *p = buf; *p; p++)
        if (*p == '_')
            *p = ' ';
    return buf;
}

static void clear(void)
{
    for (int i = 0; i < nnodes; i++)
        free(nodes[i].children);
    free(nodes);
    free(props);
    free(roots);
    free(text);
    nodes = NULL;
    props = NULL;
    roots = NULL;
    text = NULL;
    nnodes = nprops = nroots = 0;
}

static int find_node(const char *path)
{
    for (int i = 0; i < nnodes; i++)
        if (strcmp(nodes[i].path, path) == 0)
            return i;
    return -1;
}

static void add_child(int *count, int **list, int child)
{
    int *grown = realloc(*list, (size_t)(*count + 1) * sizeof **list);
    if (!grown)
        return;
    *list = grown;
    grown[(*count)++] = child;
}

/* Read and parse /dev/devices. A node whose parent path is not a node is
 * placed below the nearest ancestor that is one, or at the top. */
static int load(void)
{
    int fd = open("/dev/devices", O_RDONLY);
    if (fd < 0)
        return -1;
    char *buf = malloc(MAX_TEXT);
    size_t len = 0;
    long r;
    while (buf && len < MAX_TEXT - 1 && (r = read(fd, buf + len, MAX_TEXT - 1 - len)) > 0)
        len += (size_t)r;
    close(fd);
    if (!buf)
        return -1;
    buf[len] = '\0';
    clear();
    text = buf;
    int lines = 1;
    for (size_t i = 0; i < len; i++)
        lines += text[i] == '\n';
    nodes = calloc((size_t)lines, sizeof *nodes);
    props = calloc((size_t)lines, sizeof *props);
    if (!nodes || !props)
        return -1;
    for (char *line = text; *line;) {
        char *end = strchr(line, '\n');
        if (end)
            *end = '\0';
        char *tab = strchr(line, '\t');
        if (tab) {
            *tab = '\0';
            if (line[0] == '@') {
                struct node *n = &nodes[nnodes++];
                n->path = line + 1;
                n->title = tab + 1;
                n->first_prop = nprops;
                n->parent = -1;
            } else if (nnodes) {
                props[nprops].key = line;
                props[nprops].value = tab + 1;
                nprops++;
                nodes[nnodes - 1].nprops++;
            }
        }
        if (!end)
            break;
        line = end + 1;
    }
    char parent[256];
    for (int i = 0; i < nnodes; i++) {
        snprintf(parent, sizeof parent, "%s", nodes[i].path);
        int p = -1;
        char *slash;
        while (p < 0 && (slash = strrchr(parent, '/'))) {
            *slash = '\0';
            p = find_node(parent);
        }
        nodes[i].parent = p;
        if (p >= 0)
            add_child(&nodes[p].nchildren, &nodes[p].children, i);
        else
            add_child(&nroots, &roots, i);
    }
    return 0;
}

static int m_rows(struct model *m, int parent)
{
    return parent < 0 ? nroots : parent < nnodes ? nodes[parent].nchildren : 0;
}

static int m_child(struct model *m, int parent, int index)
{
    return parent < 0 ? roots[index] : nodes[parent].children[index];
}

static int m_columns(struct model *m)
{
    return 1;
}

/* A category shows its translated name, every other node its title. */
static const char *m_cell(struct model *m, int row, int col, char *buf, size_t size)
{
    if (row < 0 || row >= nnodes)
        return "";
    if (nodes[row].parent < 0)
        for (size_t i = 0; i < sizeof categories / sizeof categories[0]; i++)
            if (strcmp(categories[i].path, nodes[row].path) == 0)
                return _(categories[i].label);
    return nodes[row].title;
}

static const char *m_header(struct model *m, int col)
{
    return _("Device");
}

static struct model tree_model = { m_rows, m_child, m_columns, m_cell, m_header, NULL, NULL, NULL };

/* The table: the title of the node, then its properties. */
static int p_rows(struct model *m, int parent)
{
    if (parent >= 0 || selected < 0)
        return 0;
    return 1 + nodes[selected].nprops;
}

static int p_child(struct model *m, int parent, int index)
{
    return index;
}

static int p_columns(struct model *m)
{
    return 2;
}

static const char *p_cell(struct model *m, int row, int col, char *buf, size_t size)
{
    if (selected < 0)
        return "";
    if (row == 0)
        return col == 0 ? _("Description") : nodes[selected].title;
    const struct prop *p = &props[nodes[selected].first_prop + row - 1];
    return col == 0 ? key_label(p->key, buf, size) : p->value;
}

static const char *p_header(struct model *m, int col)
{
    return col == 0 ? _("Property") : _("Value");
}

static struct model prop_model = { p_rows, p_child, p_columns, p_cell, p_header, NULL, NULL, NULL };

static void show(int node)
{
    selected = node;
    view_refresh(table);
    if (node >= 0) {
        char line[320];
        snprintf(line, sizeof line, "%s", nodes[node].path);
        widget_set_text(status, line);
        printf("sysinfo: %s, %d properties\n", nodes[node].path, nodes[node].nprops);
        fflush(stdout);
    }
}

static int on_select(struct widget *w, void *args, void *arg)
{
    const struct sig_select *s = args;
    int node = s->index >= 0 && s->index < nnodes ? s->index : -1;
    if (node != selected)
        show(node);
    return 1;
}

/* Read /dev/devices again. The expanded nodes and the selection are found
 * again by their paths. */
static void refresh(void)
{
    int nexp = 0;
    char **expanded = calloc((size_t)(nnodes + 1), sizeof *expanded);
    for (int i = 0; expanded && i < nnodes; i++)
        if (nodes[i].nchildren && treeview_is_expanded(tree, i))
            expanded[nexp++] = strdup(nodes[i].path);
    char *sel = selected >= 0 ? strdup(nodes[selected].path) : strdup("system");
    selected = -1;
    if (load() < 0) {
        widget_set_text(status, _("/dev/devices cannot be read"));
        nnodes = nroots = 0;
    }
    view_set_model(tree, &tree_model);
    for (int i = 0; i < nexp; i++) {
        int n = find_node(expanded[i]);
        if (n >= 0)
            treeview_expand(tree, n, 1);
        free(expanded[i]);
    }
    free(expanded);
    int n = sel ? find_node(sel) : -1;
    free(sel);
    view_refresh(tree);
    show(n);
    if (n >= 0)
        view_select(tree, n);
    printf("sysinfo: %d nodes, %d properties, %d categories\n", nnodes, nprops, nroots);
    fflush(stdout);
}

static int on_refresh(struct widget *w, void *args, void *arg)
{
    refresh();
    return 1;
}

/* Expand or collapse every node with children. */
static int on_expand(struct widget *w, void *args, void *arg)
{
    for (int i = 0; i < nnodes; i++)
        if (nodes[i].nchildren)
            treeview_expand(tree, i, arg != NULL);
    view_refresh(tree);
    return 1;
}

static int on_quit(struct widget *w, void *args, void *arg)
{
    app_quit(app, 0);
    return 1;
}

int main(void)
{
    app = app_create();
    if (!app)
        return 1;
    textdomain("sysinfo");
    win = app_window(app, 860, 580, _("System information"));
    if (!win)
        return 1;
    struct widget *mb = menubar_new(win);
    struct widget *file = menu_new(mb, _("File"));
    struct widget *m = menu_add(file, _("Quit"), "quit");
    widget_connect(m, "clicked", on_quit, NULL);
    widget_set_accel(m, KEY_Q, WMOD_CTRL);
    struct widget *view = menu_new(mb, _("View"));
    m = menu_add(view, _("Refresh"), NULL);
    widget_connect(m, "clicked", on_refresh, NULL);
    widget_set_accel(m, KEY_F5, 0);
    menu_add_separator(view);
    widget_connect(menu_add(view, _("Expand all"), NULL), "clicked", on_expand, (void *)1);
    widget_connect(menu_add(view, _("Collapse all"), NULL), "clicked", on_expand, NULL);

    struct widget *split = splitpane_new(win, 0);
    widget_set_stretch(split, 1, 1);
    tree = treeview_new(split);
    widget_connect(tree, "selected", on_select, NULL);
    table = table_new(split);
    view_set_model(table, &prop_model);
    table_set_column_width(table, 0, 220);
    table_set_column_width(table, 1, 360);
    splitpane_set_position(split, 280);

    struct widget *sb = statusbar_new(win);
    status = statusbar_add(sb, 1);

    refresh();
    widget_focus(tree);
    app_run(app);
    app_destroy(app);
    clear();
    return 0;
}
