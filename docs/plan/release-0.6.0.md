# Release 0.6.0: virtual machine integration

This plan contains the milestones of release 0.6.0 of
`docs/plan/roadmap.md`. Milestone identifiers use the prefix `V`. Each
milestone ends with boot tests, extends or adds the design documents
listed in it, and is marked completed here when its boot tests pass. The
owner decided the scope of each goal on 2026-10-06.

## 1. Motivation and scope

minios runs in QEMU and UTM, but it cooperates with neither. The power
button of the host has no effect, and the power off of x86_64 writes to a
fixed port of the q35 machine. The clock is the RTC of the host at boot
and drifts afterwards. The resolution of the desktop is fixed at boot and
does not follow the window of QEMU or UTM. The host cannot take memory
back from a guest that does not use it. Files reach minios only through
`xfer` and `transfer` over the network.

After this plan:

- minios reads the power off and reset methods from the ACPI tables on
  both architectures, and the power button of the host shuts minios down
  in order.
- An SNTP service sets the clock at boot and corrects it gradually.
- The desktop takes the size of the window of QEMU or UTM.
- A virtio memory balloon gives memory back to the host and reports the
  memory statistics of minios.
- Folders of the host appear under `/mnt` through virtio-9p.

## 2. Fixed decisions

- uACPI changes from the barebones mode to the full mode with its AML
  interpreter, on both architectures. The kernel provides the functions of
  `uacpi/kernel_api.h`. The device tree remains the first source of the
  platform description on aarch64 (`drivers.md`).
- The power button reaches init as a request to shut down, the same
  request as `initctl shutdown`. On x86_64 it is the fixed power button
  event of ACPI. On aarch64 with ACPI it is the notification of the power
  button device through the Generic Event Device of QEMU `virt`. On
  aarch64 with a device tree it is the GPIO key of the PL061 controller.
- Host folders use virtio-9p with the protocol 9P2000.L. The QEMU of macOS
  has no vhost-user-fs, so virtiofs is not available on the host of the
  owner, and UTM shares folders through virtio-9p as well.
- The time service implements SNTP version 4 (RFC 4330). It steps the
  clock at boot and when an offset exceeds 128 ms, and corrects smaller
  offsets gradually with a new system call `adjtime`.
- The balloon driver implements inflation, deflation, the statistics
  queue and `VIRTIO_BALLOON_F_DEFLATE_ON_OOM`. It does not implement free
  page reporting or free page hinting.
- The display resize uses the display change events of virtio-gpu. The
  boot test drives them through the VNC display of QEMU, whose
  `SetDesktopSize` message changes the size of the virtual display.

## 3. Milestones

### V1. ACPI power off, reset and power button (completed 2026-10-06)

uACPI runs in its full mode on both architectures. The kernel implements
the interface of `uacpi/kernel_api.h`: memory mapping, port and memory
I/O, PCI configuration access, allocation, time, mutexes, events,
spinlocks, the SCI interrupt and deferred work in a kernel thread.
`platform_power_off` enters the sleep state S5 with the values of `\_S5`
and the PM1 control blocks of the FADT. `platform_reboot` writes the reset
register of the FADT and falls back to the methods of today. On aarch64
the power off and the reset remain PSCI calls, as the FADT of an ARM
machine requires, and the reset register is used when PSCI lacks
`SYSTEM_RESET`.

The power button sends a shutdown request to init through its control
socket.

Boot tests: `acpi_power` presses the power button through the QMP command
`system_powerdown` after the guest has started init, and requires an
orderly shutdown with the log of init and the power off of the machine.
It runs on x86_64, on aarch64 with ACPI and on aarch64 with a device tree.
`acpi_reset` restarts the machine through the reset register of the FADT
with `-no-reboot` and requires that QEMU ends with a reset. `acpi_s5`
checks the values of `\_S5` on x86_64. `acpi_s5_acpi` checks on aarch64
that the FADT of the hardware reduced ACPI requires PSCI, because that
machine has no `\_S5`.

Document: `docs/design/acpi.md`, `docs/design/init.md`.

### V2. NTP clock synchronisation (completed 2026-10-06)

The system call `adjtime` corrects the realtime clock gradually: the
kernel adds or removes up to 500 microseconds per second until the offset
is consumed, as BSD `adjtime` does. The libc function of the same name
wraps it, and the call requires root.

`ntpd` is an init service. It reads `/etc/ntp.conf` (`server NAME`, more
than one allowed, default `pool.ntp.org`), sends SNTP requests over UDP,
and computes the offset and the round trip delay from the four time
stamps. It steps the clock with `clock_settime` at start and when the
offset exceeds 128 ms, and corrects smaller offsets with `adjtime`. It
queries every 15 minutes and after a failure every minute. It writes its
results to the system log and the current state to `/run/ntpd.state`,
which `ntpd -q` prints.

Boot tests: `ntp` runs a host peer that answers SNTP requests with a time
one hour ahead, and requires the step, the slew of a small offset after a
second answer, and the refusal of a server answer with a wrong mode or a
zero transmit time. `adjtime` checks the system call with a test program
(`user/tests/adjtimetest.c`): the rate, the remaining offset that it
reports, the cancellation by a new call and by `clock_settime`, and
`EPERM` for a user other than root.

Document: `docs/design/time.md`, `docs/design/init.md`.

### V3. The desktop follows the window size (completed 2026-10-06)

The virtio-gpu driver handles the configuration change interrupt and the
event `VIRTIO_GPU_EVENT_DISPLAY`. It reads the new display information and
reports the preferred size of each scanout to the display layer. The
compositor receives the size as a display event, sets the mode at the
scale of the output, and moves and resizes its maximized windows, the
panel and the desktop as after a change in the display settings. The
display settings record whether the size follows the window, which is
the default, or remains fixed.

Boot test: `gpu_resize` starts QEMU with a VNC display on a Unix socket.
The peer connects as a VNC client and sends `SetDesktopSize` twice. The
guest log must show the two new modes, and the panel must span the new
width in a screendump.

Document: `docs/design/display.md`, `docs/design/compositor.md`.

### V4. The memory balloon (completed 2026-10-06)

A driver for virtio-balloon (device 5). It inflates the balloon by
allocating pages and reporting them to the host, and deflates it by
returning pages to the allocator, when the host changes the target in
the configuration space. It sends the memory statistics of
`VIRTIO_BALLOON_F_STATS_VQ` when the host asks for them. With
`VIRTIO_BALLOON_F_DEFLATE_ON_OOM` the allocator takes pages from the
balloon before it fails an allocation or swaps. `/dev/balloon` reports the
size of the balloon and the target.

Boot tests: `balloon` sets the target through the QMP command `balloon`,
requires the matching value of `query-balloon` and of `MemTotal` in the
guest, reads the statistics through `qom-get` of `guest-stats`, and
deflates the balloon again. `balloon_oom` inflates the balloon and then
allocates more memory than remains, which must succeed by deflation.

Document: `docs/design/balloon.md`, `docs/design/pmm.md`.

### V5. Host folders through virtio-9p (completed 2026-10-06)

A driver for virtio-9p (device 9) and a 9P2000.L client in the VFS as the
file system `9p`. It implements `Tversion`, `Tattach`, `Twalk`,
`Tlopen`, `Tlcreate`, `Tread`, `Twrite`, `Tclunk`, `Tgetattr`,
`Tsetattr`, `Treaddir`, `Tmkdir`, `Tunlinkat`, `Trenameat`, `Tsymlink`,
`Tlink` (for the link operation of the VFS),
`Treadlink`, `Tstatfs` and `Tfsync`, with several pending requests at once.
Files of the share can be read and written, mapped with `mmap` and
executed. The owner and the mode come from the host.

`mount -t 9p TAG DIR` mounts a share. At boot init mounts every 9p device
at `/mnt/TAG`. `tools/run.sh` gets the option `--share DIR`, which passes
the folder with the tag `host`.

Boot tests: `9p` shares a scratch folder of the host. The guest reads a
file of the host, writes, renames and removes files and folders, lists
the folder, follows a symbolic link, runs a program from the share and
maps a file. The post script checks the results on the host.
`9p_automount` requires the share at `/mnt/host` after the boot.

Document: `docs/design/9p.md`, `docs/design/vfs.md`.
