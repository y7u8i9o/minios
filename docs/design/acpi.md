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

Until V1 of `docs/plan/release-0.6.0.md` the aarch64 kernel built uACPI
in the barebones mode, which contains the table interface only. Since V1
both kernels build uACPI in its full mode with the AML interpreter. The
kernel Makefile compiles the uACPI sources into `build/kernel/uacpi/` with
the kernel flags without `-Werror`, so that a warning of a newer compiler in
third party code does not stop the build. The objects depend on the
Makefile, so that a change of the mode rebuilds them.

## The kernel interface of uACPI

`drivers/acpi_kernel.c` implements `uacpi/kernel_api.h` on both
architectures.

| Functions | Implementation |
|---|---|
| `get_rsdp` | the physical RSDP address that Limine returned (`bootinfo.rsdp_phys`) |
| `map`, `unmap` | an address in the direct map. Before `vmm_init` the direct map of Limine counts, which includes the "reserved, mapped" regions. Afterwards (`acpi_kernel_late`) only the direct map of the kernel counts, and other memory is mapped with `vmm_map_mmio`. The RSDP of a PC lies in such a region at 0xF52D0. A list of 64 mappings is reused, because `vmm_map_mmio` returns no address space. `unmap` does nothing |
| `log` | `klog` at the matching level with the prefix `uacpi:` |
| `pci_*` | `platform_pci_read32` and `platform_pci_write32` of segment 0. A write of 8 or 16 bits reads the word and writes it back with its part replaced |
| `io_*` | `platform_port_read` and `platform_port_write`. aarch64 has no port space, and `io_map` fails there |
| `alloc`, `free` | `kmalloc`, `kfree` |
| `get_nanoseconds_since_boot`, `stall`, `sleep` | `timer_ns`, a busy wait on `timer_ns`, `sleep_ms` |
| mutexes | kernel mutexes. A limited wait retries `mutex_trylock` every millisecond |
| events | a counter with a spinlock and a wait queue, with `waitq_wait_timeout` for a limited wait |
| `get_thread_id` | `thread_current()` |
| `disable_interrupts`, `restore_interrupts` | `arch_irq_save`, `arch_irq_restore` |
| spinlocks | kernel spinlocks. `spin_lock` disables interrupts until the unlock, so the saved flags are 0 |
| `install_interrupt_handler` | `irq_route_gsi` with the flags of `acpi_irq_flags` and a table of 8 handlers. The kernel removes no route, so `uninstall` fails |
| `schedule_work`, `wait_for_work_completion` | a ring of 32 work items for the thread `acpi`. Interrupt handlers fill the ring, so it allocates nothing |
| `handle_firmware_request` | a fatal request is logged, a breakpoint is ignored |

`acpi_irq_flags` gives the trigger mode and the polarity of the SCI. An
interrupt source override of the MADT for the GSI of the SCI decides. Without
one the SCI is level triggered and active low, as ACPI defines it. QEMU q35
overrides GSI 9 to level triggered and active high.

## The namespace

`acpi_init` (`drivers/acpi.c`) runs first in `kinit`, because the AML code
sleeps on mutexes and events. It starts the work thread, initializes
uACPI, loads the namespace from the DSDT and the SSDTs, reports the
interrupt model through `\_PIC` (the I/O APIC on x86_64, the GIC on
aarch64), runs the `_STA` and `_INI` methods of the devices and finishes
the GPEs on a machine with the fixed hardware of ACPI. It logs one line:

    namespace loaded, fixed hardware, power button through the fixed power button event of ACPI, reset register present

## Power off and reset

`platform_power_off` of x86_64 calls `acpi_power_off`, which runs `\_PTS`
and enters the sleep state S5 with the values of `\_S5` and the PM1 control
blocks of the FADT. Without ACPI, or when that fails, the fixed port of q35
follows. `platform_reboot` of x86_64 calls `acpi_reboot`, which writes the
reset register of the FADT when the FADT has one, then pulses the reset
line of the 8042.

The FADT of an ARM machine requires PSCI. `platform_power_off` and
`platform_reboot` of aarch64 call PSCI first. Only a PSCI call that returns
leads to `acpi_power_off` and `acpi_reboot`. The hardware reduced ACPI of
QEMU `virt` has no `\_S5` and no reset register.

## The power button

The power button reaches the kernel in one of three ways. Each way ends in
`acpi_power_button`, which logs `power button pressed` and sends init the
signal `SIGUSR1`, the signal of `shutdown`. init then performs its orderly
shutdown (`init.md`).

- A PC reports the button as the fixed power button event of the PM1
  registers. The handler runs in the interrupt handler of the SCI and
  defers the press to the work thread.
- The hardware reduced ACPI of QEMU `virt` has a Generic Event Device
  (`ACPI0013`). uACPI does not handle it. `acpi_init` routes each interrupt
  of its `_CRS` with `irq_route_gsi`, with the trigger mode and the
  polarity of the resource. The interrupt queues work that runs `_EVT` with
  the number of the interrupt. `_EVT` notifies the power button device
  (`PNP0C0C`) with the value 0x80, and the notify handler of that device
  reports the press. QEMU raises the interrupt for one moment, so the GIC
  line must be edge triggered, as the resource states.
- QEMU `virt` with a device tree raises line 3 of its PL061 GPIO controller
  (`gpio-keys`, `linux,code` 116). `devtree_init` records the controller,
  its interrupt and the line (`read_power_key`), because the memory of the
  device tree is reclaimed before `kinit`. `platform_power_key_init`
  (`arch/aarch64/powerkey.c`) programs the line to interrupt on its rising
  edge, and the handler clears the interrupt and defers the press.

`/dev/devices` reports the state in the properties `acpi_namespace` and
`power_button` of the platform.

## Limine base revision 4

The kernel requests Limine base revision 4. Under base revision 3, Limine
mapped only usable, bootloader reclaimable, kernel and framebuffer memory
into its direct map. Base revision 4 also maps the ACPI reclaimable, ACPI
NVS and "reserved, mapped" regions. It guarantees that every ACPI table lies
in one of them. Base revision 4 returns the RSDP as an address in the direct
map. `boot_init` subtracts the direct map offset and records the physical
address in `bootinfo.rsdp_phys`.

The new memory map type `LIMINE_MEMMAP_RESERVED_MAPPED` appears as "reserved,
mapped" in the memory map log. The physical allocator does not use it.
The direct map of the kernel, which `vmm_init` builds, does not contain it.

## Tables of the early reading

`devtree_init` (`arch/aarch64/devtree.c`) runs in `arch_init_cpu_features`
on the boot CPU, before `vmm_init`. When there is no device tree, it calls
`acpi_read_platform`. That function sets up the early table access of uACPI
with a static buffer of 4 KiB, reads the tables below into `struct devtree`,
and resets uACPI with `uacpi_state_reset`. `acpi_init` initializes uACPI
again later.

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
- A GICv3 without a GICR entry gives the redistributor of each CPU in its
  GICC entry. The kernel then takes one region from the lowest address,
  128 KiB per CPU.
- The MCFG address belongs to bus 0. The ECAM window of `struct devtree`
  starts at its first bus, so `ecam` is the MCFG address plus the first bus
  times 1 MiB.

uACPI does not define the IORT. `acpi.c` declares the table header, the node
header and the ID mapping of the IO Remapping Table specification (Arm DEN
0049). An ID mapping through an SMMU node is not followed. Without an IORT,
the ITS device ID is the PCI requester ID, as on QEMU `virt`.

The PL031 real time clock is described only in the DSDT. The early reading
does not run AML code, and its address remains the one of `virt`,
0x09010000. The PL011
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

`struct devtree` is written once by `devtree_init` before any other CPU
starts and is read without a lock afterwards. The locks of the kernel
interface of uACPI are in `locking.md` (V1 additions).

## Tests

`tests/run_qemu_test.sh` boots aarch64 `virt` without `acpi=off` when the
case has a file `acpi`.

- `acpi`: GICv3 with ITS. It runs `test=platform` and checks the log lines
  `acpi: gicv3 at 8000000 and 80a0000, its at 8080000`, the ECAM window with
  the requester ID range of the IORT, `psci through hvc` and the root mount
  from `vda`. It powers off through PSCI at the end.
- `acpi_gicv2`: GICv2 with the v2m frame and four CPUs. It runs `smptest`
  and checks `acpi: gicv2 at 8000000 and 8010000, v2m at` and the root
  mount.

Both cases reject the line `no device tree`, which `devtree_init` logs when
neither description is found.

The cases of V1 (`kernel/tests/test_acpi.c`):

- `acpi_power` starts init and waits. Its QMP script runs
  `system_powerdown`, the power button of QEMU. The log must contain
  `power button pressed`, `init: powering off` and `system powering off`,
  and QEMU must end with status 0. It runs on x86_64 (the fixed event) and
  on aarch64 with a device tree (the PL061). `acpi_power_acpi` repeats it
  on aarch64 with ACPI (the Generic Event Device) and requires the power
  button device in the line of the namespace.
- `acpi_reset` writes the reset register of the FADT on x86_64. The case
  runs with `-no-reboot`, and its QMP script waits for the event
  `SHUTDOWN`, which must have the reason `guest-reset`.
- `acpi_s5` evaluates `\_S5` on x86_64. `acpi_s5_acpi` checks on aarch64
  that the FADT of the hardware reduced ACPI requires PSCI.

The QMP script language of `tests/qmp_input.py` gained `qmp COMMAND
[JSON]`, which runs any command and prints its result, and `event NAME`,
which waits for an event and prints its data.

## Limits

- The PL031 of an ACPI machine is used at its `virt` address. The DSDT is
  not searched for it.
- The kernel removes no interrupt route of uACPI.
- A level triggered interrupt of a Generic Event Device would repeat
  until `_EVT` runs in the work thread. QEMU uses edge triggered
  interrupts.
