# The ACPI tables on aarch64

An aarch64 machine describes its devices either with a device tree or with
ACPI tables. QEMU `virt` with `acpi=off` passes a device tree. UTM,
Parallels, VMware Fusion and QEMU `virt` without `acpi=off` pass ACPI
tables, and the edk2 firmware then installs no device tree. Since D1 of
`docs/plan/drivers.md`, the kernel reads the ACPI tables when Limine passes
no device tree.

## uACPI

The tables are read with uACPI (https://github.com/uACPI/uACPI, MIT
licence). `tools/fetch_uacpi.sh` copies a release into `third_party/uacpi`
and records its version in `third_party/uacpi/VERSION`. The current version
is 6.1.1.

The aarch64 kernel builds uACPI with `UACPI_BAREBONES_MODE`. This mode
contains the table interface only. It has no AML interpreter, no events and
no namespace. The kernel Makefile compiles the uACPI sources into
`build/aarch64/kernel/uacpi/` with the kernel flags without `-Werror`, so
that a warning of a newer compiler in third party code does not stop the
build. The x86_64 kernel does not build uACPI.

In the barebones mode uACPI needs four kernel functions. They are in
`kernel/arch/aarch64/acpi.c`.

| Function | Implementation |
|---|---|
| `uacpi_kernel_get_rsdp` | the physical RSDP address that Limine returned (`bootinfo.rsdp_phys`) |
| `uacpi_kernel_map` | the address in the direct map, after a check that the range lies in a memory map entry that the direct map covers |
| `uacpi_kernel_unmap` | nothing, because the direct map is permanent |
| `uacpi_kernel_log` | `klog` at the matching level with the prefix `uacpi:` |

## Limine base revision 4

The kernel requests Limine base revision 4. Under base revision 3, Limine
mapped only usable, bootloader reclaimable, kernel and framebuffer memory
into its direct map. Base revision 4 also maps the ACPI reclaimable, ACPI
NVS and "reserved, mapped" regions. It guarantees that every ACPI table lies
in one of them. Base revision 4 returns the RSDP as an address in the direct
map. `boot_init` subtracts the direct map offset and records the physical
address in `bootinfo.rsdp_phys`.

The new memory map type `LIMINE_MEMMAP_RESERVED_MAPPED` is named "reserved,
mapped" in the memory map log. The physical allocator does not use it.
The direct map of the kernel, which `vmm_init` builds, does not contain it.

## What the kernel reads

`devtree_init` (`arch/aarch64/devtree.c`) runs in `arch_init_cpu_features`
on the boot CPU, before `vmm_init`. When there is no device tree, it calls
`acpi_read_platform`. That function sets up the early table access of uACPI
with a static buffer of 4 KiB, reads the tables below into `struct devtree`,
and resets uACPI with `uacpi_state_reset`. No uACPI function is called
afterwards.

| Table | Fields of `struct devtree` |
|---|---|
| MADT (`APIC`) | `gicd` and `gic_version` from the GICD entry. `gicc` from the first GICC entry. `v2m` from the first GIC MSI frame. `gicr` and `gicr_size` from the first GICR entry. `its` from the first GIC ITS entry |
| MCFG | `ecam`, `ecam_size`, `bus_start` and `bus_end` from the allocation of PCI segment 0 |
| IORT | `msi_rid_base`, `msi_base` and `msi_length` from the first ID mapping of a root complex node that leads to an ITS group node |
| FADT | `psci_smc` from the PSCI flags of `arm_boot_arch` |
| GTDT | no field. A virtual timer interrupt other than `IRQ_TIMER` (27) is reported |

Some details of the MADT:

- A GICD entry with version 0 leaves the version to the other entries. A
  GICR entry or a redistributor address in the GICC entries then means a
  GICv3, else a GICv2.
- A GICv3 without a GICR entry names the redistributor of each CPU in its
  GICC entry. The kernel then takes one region from the lowest address,
  128 KiB per CPU.
- The MCFG address belongs to bus 0. The ECAM window of `struct devtree`
  starts at its first bus, so `ecam` is the MCFG address plus the first bus
  times 1 MiB.

uACPI does not define the IORT. `acpi.c` declares the table header, the node
header and the ID mapping of the IO Remapping Table specification (Arm DEN
0049). An ID mapping through an SMMU node is not followed. Without an IORT,
the ITS device ID is the PCI requester ID, as on QEMU `virt`.

The PL031 real time clock is described only in the DSDT, which needs the AML
interpreter. Its address remains the one of `virt`, 0x09010000. The PL011
console is used at its `virt` address before the tables are read. The SPCR
is not read.

The log lines of the platform name their source: `acpi: gicv3 at ...` or
`device tree: gicv3 at ...`, followed by the ECAM window and `psci through
hvc` or `psci through smc`.

## PSCI conduit

`platform_power_off` and `platform_reboot` call PSCI. The conduit was always
`hvc` before D1. It is now `smc` when the FADT does not set
`PSCI_USE_HVC`, or when the `psci` node of the device tree has the method
`smc`. QEMU `virt` without firmware at EL2 or EL3 uses `hvc` in both
descriptions.

## Locking

No new lock. `struct devtree` is written once by `devtree_init` before any
other CPU starts and is read without a lock afterwards. The uACPI state and
its static buffer are used only inside `acpi_read_platform` on the boot CPU.

## Tests

`tests/run_qemu_test.sh` boots aarch64 `virt` without `acpi=off` when the
case has a file named `acpi`.

- `acpi`: GICv3 with ITS. It runs `test=platform` and checks the log lines
  `acpi: gicv3 at 8000000 and 80a0000, its at 8080000`, the ECAM window with
  the requester ID range of the IORT, `psci through hvc` and the root mount
  from `vda`. It powers off through PSCI at the end.
- `acpi_gicv2`: GICv2 with the v2m frame and four CPUs. It runs `smptest`
  and checks `acpi: gicv2 at 8000000 and 8010000, v2m at` and the root
  mount.

Both cases reject the line `no device tree`, which `devtree_init` logs when
neither description is found.

## Limits

- The AML interpreter is not built. Devices that only the DSDT describes,
  such as the PL031 and the GPIO power button, use the `virt` addresses or
  are not used.
- The x86_64 kernel does not read ACPI tables. Its power off uses the ACPI
  PM1a port of the q35 machine.
