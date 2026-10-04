# System information

`sysinfo` shows the machine and its devices. The kernel describes them in
the read-only node `/dev/devices`. The program reads the node and shows it
as a tree with a property table.

## /dev/devices

The text is made when the node is opened (`kernel/drivers/devinfo.c`). It is
a sequence of nodes:

    @PATH<TAB>TITLE
    KEY<TAB>VALUE
    ...

- **Paths.** `PATH` is a list of names separated by `/`. The node
  `usb/xhci0/port5/device` is a child of `usb/xhci0/port5`.
- **Keys.** Keys are lower case words joined by `_`, and their meaning is
  stable.
- **Titles and values.** These are English text.
- **Tabs and newlines.** In a title or a value, they are replaced by spaces.

The writer grows its buffer from 64 KiB to at most 4 MiB.

| Category | Written by | Content |
|---|---|---|
| `system` | `drivers/devinfo.c` | release, build, architecture, CPUs, uptime, clock, bootloader, command line, boot disk, kernel addresses, configuration |
| `firmware` | `drivers/devinfo.c`, `drivers/acpi_tables.c`, `drivers/smbios.c`, aarch64 `describe.c` | firmware type, ACPI table list, SMBIOS firmware, system, baseboard, chassis, processor sockets and memory modules, device tree model |
| `cpu` | `arch/x86_64/cpu.c`, `arch/aarch64/describe.c` | x86_64: CPUID vendor, brand, family, features, caches. aarch64: MIDR, ID registers, features, caches. Both: one node per CPU |
| `platform` | `arch/*/describe.c`, `apic.c`, `gic.c`, `its.c` | interrupt controllers, PCIe window, MSI mapping, serial port, clock device, PSCI, power off and reboot |
| `memory` | `drivers/devinfo.c` | RAM, free and used memory, swap, every memory map entry |
| `pci` | `drivers/pci.c` | IDs with vendor and device names, class, revision, command register, BAR addresses and sizes, interrupt pin, capabilities (power management, MSI, MSI-X, PCI Express link), bound driver |
| `usb` | `drivers/usb/xhci.c`, `usb.c`, `hid.c` | controllers, every port with its status, devices with their strings and descriptors, interfaces with endpoints, drivers and input devices |
| `input` | `input/core.c` | every input device with bus, IDs, keys, buttons, axes, repeat, readers |
| `storage` | `block/part.c` | disks and GPT partitions with types, GUIDs and sizes, the filesystem and mount points of each, and every mounted filesystem with its space |
| `display` | `drivers/fbdev.c`, `virtio_gpu.c` | resolution, scale, pixel format, driver, GPU scanout |
| `audio` | `audio/pcm.c`, `virtio_snd.c` | PCM devices and their streams |
| `network` | `net/netif.c` | interfaces with driver, MAC, MTU, IPv4 configuration and counters |

The SMBIOS structure table and the list of ACPI table headers are copied
once at start-up by `devinfo_init`, with `vmm_copy_from_phys`. The ACPI list
is read from the RSDP on both architectures without uACPI.

Drivers record their name for the description: `struct pci_dev.driver`,
`struct netif.driver`, the `describe` operations of `struct fb_gpu_ops` and
`struct pcm_ops`.

### Shared functions added for the description

- `kernel/lib/string.c`: `strlcat`, `memchr`, `strstr`.
- `kernel/include/lib/endian.h`: `get_le16`, `get_le32`, `get_le64`.
- `kernel/lib/guid.c`: `guid_format`, `guid_parse`, `guid_is_zero`. These
  were previously private to `block/part.c`.
- `mm/vmm.c`: `vmm_copy_from_phys`. It reads through the direct map, or
  maps the range temporarily and unmaps it.
- `devinfo_append` and `devinfo_format_size` for lists and sizes in values.
- `usb_speed_name`.
- `vfs_for_each_mount` walks the mount table with the mounts pinned, for
  `/dev/mounts` and the storage part. `struct mount` records the source of
  the mount.
- `timer_clock_hz`.

### Locking

- **The text.** It is built in the opening thread. Each part takes the
  lock of its own subsystem: `input_devices_lock` and `input_dev.lock`,
  `part_lock`, `fb_mode_lock`, `fbdev_lock`, the GPU and sound mutexes,
  `netif_lock`.
- **USB.** `usb_topology_lock` (mutex) protects the devices published on
  the USB ports. The controller threads take it to publish a device after
  its enumeration and to remove it before its slot is freed.
- **Tables filled at start-up.** These are read without a lock: the PCI
  table, the PCM device table, the copied SMBIOS and ACPI data, and
  `cpu_features`.

## The program

`user/apps/sysinfo.c`:

- **Tree.** The tree on the left lists the categories under their
  translated names, and the nodes below them under their titles.
- **Table.** The table on the right lists the description and the
  properties of the selected node. Property names come from a table of the
  known keys, translated through the `sysinfo` domain. The numbered keys
  `barN`, `endpoint_XX` and `absolute_axis_N` have their own labels.
  Values are shown as the kernel wrote them.
- **Status bar.** It shows the path of the node.
- **Commands.** View > Refresh (F5) reads the node again, and restores the
  expanded nodes and the selection by their paths. View > Expand all and
  Collapse all act on every node.

The program prints `sysinfo: N nodes, M properties, K categories` after
each read and `sysinfo: PATH, N properties` for each selection. The test
checks these lines.

`sysinfo --json` writes the tree to standard output instead of opening a
window, with `libjson` (`json.md`). The document is an array of the
categories. Each node is an object with `path`, `title`, `properties` (an
object of the keys and their values) and `children` (an array of nodes).

It is in the package `diagnostics`, in the launcher as "System information",
with the icon `microchip`.

## Tests

- `devices` (both architectures, `qemu-xhci` with a USB keyboard and
  tablet): the self test checks the format, the required nodes, a USB
  device, the bound drivers, and the ACPI table list when there are ACPI
  tables. It writes the text to the serial log with `console_write_user`.
- `devices_acpi` (aarch64, ACPI tables, GICv2, `nec-usb-xhci`): the same
  checks with the ACPI table list.
- `sysinfo_json`: runs `sysinfo --json` and checks the start of the
  document and the node `storage/filesystems`.
- `gui_sysinfo`: starts the program, selects the firmware node with Down,
  refreshes with F5 and closes the window. The `qmp` script of the case
  saves a screenshot as `sysinfo.ppm` in the case directory, with the
  `screendump` command added to `tests/qmp_input.py`.

## Limits

- The program shows the state at the last read. It does not refresh by
  itself.
- Values are not translated.
- The PL031 and other devices that only the DSDT describes are not listed.
  The AML interpreter is not built.
