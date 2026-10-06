# Hardware drivers

This plan adds the drivers that machines other than QEMU with virtio
devices need. Milestone identifiers use the prefix `D`. Each milestone ends
with boot tests and a document in `docs/design/`, and is marked completed
here when its boot tests pass.

## 1. Motivation and scope

minios runs on QEMU with virtio devices and, on aarch64, with a device
tree. Other virtual machines and real computers differ in five ways.

- UTM, Parallels and VMware Fusion start aarch64 machines with ACPI tables
  and no device tree. Without a device tree the aarch64 kernel finds no PCI
  devices.
- Keyboards and mice are USB devices behind an xHCI controller in UTM,
  VMware, Parallels, VirtualBox and on real computers. minios has no USB
  driver.
- Disks are NVMe or SATA (AHCI) devices outside virtio.
- Network cards are Intel e1000e devices outside virtio.
- Sound cards are Intel HD Audio devices outside virtio.

Third party code is used where an existing implementation fits the kernel
with little adaptation. uACPI (MIT licence) reads the ACPI tables. No
open source USB host stack in C has a usable xHCI driver: TinyUSB has EHCI
and OHCI only, and CherryUSB publishes its xHCI driver only as a compiled
library for Phytium processors. The USB, storage, network and sound drivers
are therefore written from the public specifications. Code from Linux is
not used.

Since 2026-10-05 a driver adapts an existing implementation under a
permissive licence where the adaptation costs less than new code
(`release-0.5.0.md`). The xHCI driver of D2 and the NVMe driver of D3
were written from the specifications before that decision.

## 2. Fixed decisions

- uACPI is a copy in `third_party/uacpi`, fetched by `tools/fetch_uacpi.sh`
  and built into the aarch64 kernel in the barebones mode, which provides
  the table interface without the AML interpreter.
- The device tree remains the first source of the platform description. The
  ACPI tables are read when Limine passes no device tree.
- The USB drivers are generic code in `kernel/drivers/usb/` and are built on
  both architectures.
- USB keyboards, mice and tablets report to the input core and appear as
  `/dev/input/eventN`, like the PS/2 and virtio devices.
- Each driver takes its interrupt through MSI-X, else through MSI, and polls
  when the platform delivers neither.

## 3. Milestones

### D1. The ACPI tables on aarch64 through uACPI (completed 2026-10-04)

`tools/fetch_uacpi.sh` copies uACPI into `third_party/uacpi`. The aarch64
kernel builds it with `UACPI_BAREBONES_MODE`. The kernel requests Limine
base revision 4, in which Limine maps the ACPI tables into the direct map
and returns the RSDP. When Limine passes no device tree, `devtree_init`
reads the tables through uACPI:

- MADT: the GIC distributor and its version, the GICv2 CPU interface, the
  GICv2m MSI frame, the GICv3 redistributor region and the ITS.
- MCFG: the PCIe ECAM window and its bus range.
- IORT: the translation of PCI requester IDs to ITS device IDs.
- FADT: the PSCI conduit, `hvc` or `smc`.

The PSCI conduit of the device tree is read from the `psci` node, which
replaces the fixed `hvc`.

Boot tests: `acpi` boots aarch64 virt with ACPI and a GICv3, and
`acpi_gicv2` with ACPI and a GICv2. Both mount the root from virtio-blk
through MSI interrupts and check that the platform came from the ACPI
tables. The harness file `acpi` selects a machine without `acpi=off`.

Document: `docs/design/acpi.md`.

### D2. xHCI with USB keyboards, mice and tablets (completed 2026-10-04)

An xHCI driver finds the controllers of PCI class 0c03 interface 30. It
takes the controller from the firmware, resets it and sets up the device
context array, the scratchpad buffers, the command ring and one event ring.
Its interrupt is MSI-X, MSI or polling. A thread per controller resets the
ports, enumerates the devices that are connected at boot and later, and
releases the slots of disconnected devices.

The USB core reads the device, configuration and string descriptors and
selects the configuration. The HID driver binds to every HID interface
with an interrupt IN endpoint:

- A boot keyboard uses the boot protocol. Its reports are compared with the
  previous report to produce presses and releases.
- Other HID interfaces use the report protocol. A parser of the report
  descriptor locates the buttons, the relative axes X, Y and the wheel, the
  absolute axes X and Y with their logical ranges, and keyboard fields.

Each HID interface registers one input device with the bus type `BUS_USB`.

Boot tests: `usb_hid` attaches `qemu-xhci` with `usb-kbd`, `usb-tablet` and
`usb-mouse` and no virtio input device, sends a key, an absolute position,
a button and a relative movement through QMP, and checks the events on
`/dev/input`. `usb_hid_msi` repeats it with MSI instead of MSI-X,
`usb_hid_polled` without an interrupt, and `usb_hid_nec` with
`nec-usb-xhci`, the controller of UTM. `usb_hid_acpi` runs on aarch64 with
ACPI tables, a GICv2 and `nec-usb-xhci`, the machine that UTM starts. The
harness files `usb` and `qmp` attach the USB devices and send the input.

Document: `docs/design/usb.md`.

### D3. NVMe (completed 2026-10-05)

A driver for NVMe controllers (PCI class 0108 interface 02): the admin
queue, one I/O queue pair per CPU or one in total, identify, namespaces as
block devices. Boot test with a QEMU `nvme` device as the root disk.
Done as R2 of `release-0.5.0.md`. Document: `docs/design/nvme.md`.

### D4. AHCI (completed 2026-10-05)

A driver for AHCI SATA controllers (PCI class 0106 interface 01): port
detection, command lists, received FIS areas, READ and WRITE DMA EXT for
disks, and ATAPI packet commands for CD drives. Boot test with the q35
SATA controller as the root disk. Done as R3 of `release-0.5.0.md` by
adapting the AHCI code of edk2. Document: `docs/design/ahci.md`.

### D5. USB mass storage (completed 2026-10-05)

The bulk-only transport of the USB mass storage class with the SCSI
commands INQUIRY, READ CAPACITY, READ and WRITE, registered as block
devices. USB hubs, which real computers place between the root ports and
the devices. Boot tests with `usb-storage` and with `usb-hub`. Done as R4
of `release-0.5.0.md` by adapting the mass storage and hub drivers of
edk2. Document: `docs/design/usb.md`.

### D6. e1000e (completed 2026-10-06)

A driver for the Intel 82574L (e1000e) network card: the receive and
transmit descriptor rings, the link state and the MAC address from the
EEPROM, registered with the network core. Boot test with the QEMU `e1000e`
device against the network peer. Done as S1 of
`release-0.7.0.md`. Document: `docs/design/e1000e.md`.

### D7. Intel HD Audio

A driver for Intel HD Audio controllers: the CORB and RIRB command rings,
codec enumeration, an output path from a converter to a pin, and stream
descriptors with buffer descriptor lists, exposed as a PCM device to
`audiod`. Boot test with `intel-hda` and `hda-duplex` and the wav audio
backend.
