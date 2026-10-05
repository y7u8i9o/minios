# USB: xHCI, enumeration, HID, mass storage and hubs

Since D2 of `docs/plan/drivers.md`, minios drives xHCI host controllers and
the USB keyboards, mice and tablets connected to them. These devices report
to the input core and appear as `/dev/input/eventN`, like the PS/2 and
virtio input devices. Since R4 of `docs/plan/release-0.5.0.md` (D5), USB
disks and CD drives with the bulk-only transport and USB hubs work as
well. The code is generic and runs on both architectures.

| File | Content |
|---|---|
| `kernel/drivers/usb/xhci.c` | the xHCI controller driver and the controller thread |
| `kernel/drivers/usb/usb.c` | the USB core: descriptors, configuration, binding |
| `kernel/drivers/usb/hid.c` | the HID class driver and the report descriptor parser |
| `kernel/drivers/usb/msc.c` | the mass storage class driver with the bulk-only transport (R4) |
| `kernel/drivers/usb/hub.c` | the hub class driver (R4) |
| `kernel/drivers/usb/usb.h` | the declarations shared by these files |
| `kernel/include/drivers/usb.h` | `usb_init` and `usb_device_count` |

The driver is written from the eXtensible Host Controller Interface
specification 1.2, the USB 2.0 specification (chapter 9), the Device Class
Definition for HID 1.11 and the HID Usage Tables 1.4. No existing USB stack
was used. TinyUSB has no xHCI driver, and CherryUSB publishes its xHCI driver
only as a compiled library.

The mass storage and hub drivers of R4 adapt edk2 (BSD-2-Clause-Patent,
`third_party/edk2/`): `UsbMassBot.c` for the bulk-only transport,
`UsbHub.c` and the port enumeration of `UsbEnumer.c` for hubs, and the slot
context of devices behind hubs from `XhcInitializeDeviceSlot` and
`XhcConfigHubContext` of `XhciSched.c`. TinyUSB's mass storage driver was
considered first and not used: it is an asynchronous state machine without
the reset recovery and the status wrapper checks of the bulk-only
specification, and its configuration repeats TEST UNIT READY without end
on a CD drive without a medium.

## Start

`usb_init` runs in `kinit` after `virtio_input_init`. It looks for PCI
functions of class 0c, subclass 03 and interface 30, at most four. For each
one `xhci_start`:

1. Reads the size of BAR 0 with `pci_bar_size`, enables bus mastering and
   maps the BAR as device memory.
2. Reads the capability registers: the slots, the ports, the context size
   (32 or 64 bytes) and the extended capabilities. A controller without
   64 bit addressing is refused, because the physical allocator may return
   pages above 4 GiB.
3. Takes the controller from the firmware through the USB legacy support
   capability. It sets the OS owned flag and waits up to one second for
   the firmware to clear its flag. It then disables the SMI enables. It
   records the USB major revision of each port from the supported protocol
   capabilities.
4. Stops and resets the controller, and checks that it supports 4 KiB
   pages.
5. Allocates the device context base address array, the scratchpad buffers
   that the controller asks for, the command ring and one event ring
   segment with its segment table. Each of these is one page.
6. Sets up the interrupt: MSI-X with one vector, else MSI with one vector
   (`pci_msi_enable`, added for this driver), else none. Without an
   interrupt, the controller thread polls the event ring every 10 ms.
7. Starts the controller and powers the ports that have power switches.
8. Starts the thread `xhciN`.

`usb_init` then waits up to two seconds until every controller thread has
handled the ports that were connected at boot. The display server opens
the input devices once at its start, and the devices present at boot
therefore exist before init starts it.

The boot log names the controller, its interrupt and its size:
`00:02.0: xhci 1.00, msi-x interrupt, 8 ports, 32 slots, 32 byte contexts`.

## Rings

A ring is one page of 256 TRBs. The last TRB is a link to the start with the
toggle cycle flag. `ring_push` writes the parameter and the status word
first. It then writes the control word with the producer cycle bit with a
release store, because that bit hands the TRB to the controller. A doorbell
write follows a `wmb`.

The event ring is consumed by `process_events` up to the first TRB whose
cycle bit differs from the consumer cycle state. The dequeue pointer is then
written with the event handler busy flag, which clears it. Three events are
handled:

- Command completion: the completion code and the slot ID of the command
  whose TRB address matches the command that is waited for. A completion
  that arrives after its wait timed out does not complete a later command.
- Transfer event: for endpoint 0 it completes the control transfer that
  the thread waits for. For an interrupt endpoint it passes the report to
  the class driver and queues the next transfer. On any other completion
  code it marks the endpoint halted and wakes the thread.
- Port status change: it marks the port and wakes the thread.

The interrupt handler clears the interrupt flags and calls `process_events`.
Every wait for a completion also calls it on each pass, at least every
10 ms. A completion therefore arrives also when the interrupt is lost.

## The controller thread

The thread issues every command and every control transfer of its
controller. Commands and control transfers are therefore never
concurrent, and one completion record for commands and one per slot for
endpoint 0 are enough. The thread first handles every port, then waits for
port status changes and halted endpoints.

For a port, `handle_port` clears the change bits. It detaches the device of
a port that is no longer connected, or that reports a new connection. For a
connected port without a device it waits 100 ms (the connect debounce
interval) and enables the port:

- A USB 2 port is enabled by a port reset.
- A USB 3 port enables itself when its link trains. A warm reset follows
  if it is not enabled within 200 ms.

`port_attach` then:

1. Enables a slot.
2. Allocates the device context, the input context, the ring of endpoint 0
   and a page for the data stage of control transfers.
3. Issues Address Device with the speed and the root port. The maximum
   packet size of endpoint 0 is 512 at super speed, 64 at high speed and 8
   otherwise.
4. Reads the first 8 bytes of the device descriptor after the 10 ms
   recovery interval. If the descriptor states another packet size,
   Evaluate Context sets it.
5. Passes the device to `usb_enumerate`.

`port_detach` removes the slot from the slot table under the lock, so that
its events are ignored from then on, and marks its endpoints inactive. It
calls `usb_disconnect`, disables the slot and frees its pages.

A control transfer is a Setup TRB with the request as immediate data, a
Data TRB when there is data, and a Status TRB in the opposite direction
with the interrupt on completion flag. The data passes through the slot's
data page, so a transfer has at most 4096 bytes. A stall returns `-EPIPE`
after Reset Endpoint and Set TR Dequeue Pointer to the enqueue position. A
transfer without completion within two seconds returns `-ETIMEDOUT` after
Stop Endpoint and the same Set TR Dequeue Pointer.

`xhci_interrupt_in` adds an interrupt IN endpoint with Configure Endpoint.
The input context contains a copy of the slot context with the last valid
context index raised, and the new endpoint context. The endpoint context
has the maximum packet size, an average TRB length and a maximum payload of
one packet, and the interval:

- At full and low speed `bInterval` counts frames of 1 ms. The interval is
  the largest power of two of 125 us units that does not exceed it, from
  2^3 to 2^10.
- At high and super speed the interval is `bInterval - 1`.

The endpoint then has one Normal TRB queued at a time, with the interrupt on
completion and interrupt on short packet flags. The transfer length is the
longest input report of the interface or the maximum packet size,
whichever is larger, at most 1024 bytes. An endpoint that fails is reset by
the thread and queued again, up to five consecutive failures.

## The USB core

`usb_enumerate` reads the device descriptor, the product string and the
first configuration (up to 4096 bytes), and sets the configuration. The
product string is read in the first language that string descriptor 0
lists, and is reduced to ASCII. It names the device in the log and the input
device. A device without a product string is named `USB device VVVV:PPPP`.
The core binds the HID driver to the first alternate setting of every HID
interface, the mass storage driver to every mass storage interface and the
hub driver to every hub interface. The log line is
`port 5: QEMU USB Keyboard, 0627:0001, high speed, slot 1, 1 interface`.
The port of a device behind hubs is a path, as `5.2` for port 2 of the hub
on root port 5.

## Bulk transfers (R4)

`xhci_bulk_open` configures a bulk endpoint with its own transfer ring and
a bounce block of 64 KiB, 16 contiguous pages that the buddy allocator
aligns to their size, so one Normal TRB with interrupt on short packet and
on completion describes a whole transfer. `xhci_bulk` copies the data,
queues the TRB, rings the doorbell and waits for the transfer event, which
gives the completion code and the residual length. A stall returns
`EPIPE` and leaves the endpoint halted until `xhci_clear_halt` resets the
endpoint in the controller, moves its dequeue pointer and sends
CLEAR_FEATURE ENDPOINT_HALT to the device. A transaction error resets the
endpoint, and a timeout stops it.

The class drivers call `xhci_bulk` and `xhci_control` from the threads of
the block layer. `xhci.cmd_lock` serializes the commands of a controller,
and `slot.ctrl_lock` the control transfers of a device. A disconnected
device is marked `gone` before its drivers unbind. Every wait of a transfer
on it then ends with `ENODEV`, and no transfer starts.

## Mass storage (R4)

`msc_probe` binds an interface of class 08, subclass 06 (SCSI) and
protocol 50h (bulk only). It opens the first bulk IN and bulk OUT
endpoints, with the burst of their SuperSpeed companion descriptors, and
asks GET MAX LUN. A stall or a value above 15 means one logical unit. Each
logical unit registers through `block/scsi.c` (`block.md`): a disk as
`sdX`, a CD drive as `srN`.

A command is the command block wrapper of 31 bytes on bulk OUT, the data
stage in transfers of at most 64 KiB, which ends early at a short
transfer, and the command status wrapper of 13 bytes on bulk IN. The
status stage follows also after a failed data stage, and it is tried three
times, with a stall of bulk IN cleared in between. A wrapper with a wrong
signature, tag or length, and the status 2 (phase error), cause a reset
recovery: the class request Bulk-Only Mass Storage Reset, 100 ms, and the
clearing of both endpoints. A stalled command block causes a reset
recovery as well, and a stall in the data stage is cleared. The status 1
(command failed) becomes the CHECK CONDITION of the SCSI module, which
asks REQUEST SENSE. `msc.lock` serializes the commands of all logical
units of an interface.

On disconnection the logical units are detached: their block devices
remain registered without sectors and fail with `ENODEV`. A disk that
registers after the partition scan of the boot, such as a USB stick
connected later, has its partition table read by `part_add_disk`.

## Hubs (R4)

`hub_probe` binds a hub interface. It reads the hub descriptor (type 29h,
or 2Ah for a SuperSpeed hub), marks the xHCI slot as a hub with its number
of ports and the think time of its transaction translator (Configure
Endpoint with the slot context, xHCI 4.6.7), sets the hub depth of a
SuperSpeed hub, powers every port, waits the power-on time of the
descriptor (at least 20 ms) and acknowledges the hub status. It then
enumerates the ports that have a device, and starts the status change
endpoint, an interrupt IN endpoint with one bit for the hub and one for
each port. Hubs beyond the fifth tier are refused, and ports above 15,
which a route string cannot name, are not used.

The report callback of the status change endpoint runs under `xhci.lock`
and adds the bits to the hub's change map. It marks hub work for the
controller thread, which calls `hub_service` for every hub. A port whose
status reports a change of the connection, of the enable state, of the
over-current state or a completed reset loses its device, and a connected
device is enumerated again: 100 ms of debounce, a port reset of 20 ms
followed by the wait for the reset change and 10 ms of recovery, the speed
from the port status, and up to three attempts. The change bits are
cleared after the enumeration, as in edk2, which also clears the enable
change that some hubs set with a reset.

A device behind a hub receives a slot context with the route string (one
nibble per tier, the port of the hub at that tier), the root port of the
first hub, and for a low or full speed device behind a high speed hub the
slot and the port of that hub as its transaction translator. A device
behind a full or low speed path inherits the translator of its hub. The
devices of a hub are published under `usb_topology_lock` and appear in
`/dev/devices` below the hub. A disconnected hub removes the devices
behind it before its own slot is freed.

## HID

`hid_probe` binds an interface with an interrupt IN endpoint.

- A boot keyboard (subclass 1, protocol 1) is set to the boot protocol and
  an idle rate of 0. The driver describes its 8 byte report with the same
  field structures the parser produces: eight variables for the modifier
  usages 0xe0 to 0xe7, and an array of six key slots.
- Every other HID interface uses the report protocol. A boot mouse is set
  to the report protocol explicitly. The driver reads the report descriptor
  and parses it.

The parser follows the item structure of the HID specification:

- Global items: usage page, logical minimum and maximum, report size, count
  and ID, and Push and Pop with a stack of four.
- Local items: usages, usage minimum and maximum. A usage with four data
  bytes carries its page.
- Main items: an Input item produces fields. Every main item ends the local
  items.

A logical maximum that reads as negative above a non-negative minimum was
written without its sign byte and is read as unsigned.

An Input item of the variable kind produces one field per report count,
each with one usage. An Input item of the array kind produces one field
whose slots carry usage indexes. A constant item only advances the bit
position of its report ID. The fields are mapped as follows.

| Usage | Event |
|---|---|
| keyboard page 0x07 | the key code of the usage table in `hid.c` |
| button page 0x09, buttons 1 to 8 | `BTN_LEFT` to `BTN_TASK` |
| desktop X and Y, relative | `REL_X`, `REL_Y` |
| desktop X and Y, absolute | `ABS_X`, `ABS_Y` with the logical range |
| desktop wheel, relative | `REL_WHEEL` |
| consumer AC Pan, relative | `REL_HWHEEL` |
| desktop system power, sleep, wake up | `KEY_POWER`, `KEY_SLEEP`, `KEY_WAKEUP` |
| consumer next, previous, stop, play/pause, mute, volume | the media key codes |

Keys are handled as a set. Each report computes the set of key codes that
it reports as down. The difference to the previous set produces presses and
releases. A report changes only the key codes that fields with its report
ID can produce, so a report of another ID does not release them. A keyboard
array that reports ErrorRollOver, POSTFail or ErrorUndefined leaves the key
set unchanged. Relative axes are reported when they are not 0, absolute axes
always, and every report ends with `SYN_REPORT`.

Each bound interface registers one input device with the bus type `BUS_USB`
(3, added to `minios/input.h`), the vendor and product IDs and the device
release. Its name is the product string, followed by `(interface N)` for an
interface other than 0. A device with keys gets the software key repeat of
500 and 33 ms.

The input core cannot remove a device. `hid_disconnect` releases the keys
that are down and puts the device on a free list. The next interface with
the same name and the same capabilities takes it from there instead of
registering a new one, so a reconnected keyboard is the same
`/dev/input/eventN` as before.

## Locking

`xhci.lock` (spinlock) protects the command ring and its completion fields,
the event ring dequeue state, the slot table, the rings and endpoint state
of every slot, the pending ports, the halted endpoint flag and the flag of
the initial enumeration. The interrupt handler and the controller thread
take it. The HID report callback runs under it and calls the input core,
which gives the order `xhci.lock -> input_dev.lock`. `waitq.lock` is taken
under it to wake waiters. The table of the device on each port is used by
the controller thread only.

`hid_free_lock` (spinlock) protects the free list of input devices of
disconnected interfaces. It is a leaf.

R4 adds `xhci.cmd_lock`, `slot.ctrl_lock` and `msc.lock` (mutexes) and the
change map of a hub, which `locking.md` describes with their order.

## Tests

`tests/run_qemu_test.sh` attaches an xHCI controller with USB devices for a
case with a file named `usb`. A file named `qmp` is a script of
`tests/qmp_input.py`. That tool connects to the QMP socket of QEMU, waits for
a line of the serial log and sends input events with `input-send-event`.
QEMU routes them to its USB devices as it routes the events of a display
window.

The self test `usb_hid` (`kernel/tests/test_usb.c`) checks that the three
QEMU devices are input devices with the bus type `BUS_USB` and the expected
capabilities. It then prints `usb: devices ready`, which makes the script
send the key `a`, the absolute position 10000,20000, a click of the left
button and the relative movement 5,-3. The test reads them from
`/dev/input`. QEMU sends the button to the tablet or to the mouse, whichever
its input core selected last, and the test accepts either.

| Case | Controller | Interrupt | Architectures |
|---|---|---|---|
| `usb_hid` | `qemu-xhci` | MSI-X | both |
| `usb_hid_msi` | `qemu-xhci,msix=off,msi=on` | MSI | both |
| `usb_hid_polled` | `qemu-xhci,msix=off,msi=off` | none, polled | both |
| `usb_hid_nec` | `nec-usb-xhci`, the controller of UTM | MSI-X | both |
| `usb_hid_acpi` | `nec-usb-xhci` on virt with ACPI tables and a GICv2 | MSI-X through GICv2m | aarch64 |

`usb_hid_acpi` is the machine that UTM starts for an aarch64 guest without
changes to its settings.

The QEMU 8.2 of Ubuntu 24.04 has the properties `msi` and `msix` only on
`nec-usb-xhci`, not on `qemu-xhci`, and cannot start `usb_hid_msi` and
`usb_hid_polled`. The same cases with `nec-usb-xhci` pass on such a host.

R4 adds these cases. The harness file `diskif` with `usb` attaches the root
disk as `usb-storage` on an xHCI controller of its own, and `usb@PORT` on
the controller of the `usb` file at a port path. A word `DEVICE@PORT` of
the `usb` file attaches a device at a port path, and a line `usb iso DIR`
or `usb empty` of the `cd` file a USB CD drive (`usb-bot` with `scsi-cd`).

| Case | Content | Architectures |
|---|---|---|
| `usb_storage` | the `blk` checks on a `usb-storage` disk | both |
| `usb_root` | the root file system on a `usb-storage` disk, without `root=` | both |
| `usb_cd` | the `cdrom` checks on an ISO image in a USB CD drive and an empty USB CD drive | x86_64 |
| `usb_hub` | a keyboard on port 1 and a disk on port 2 of a `usb-hub`, the `blk` checks on the disk, and no disconnection | both |

## Limits

- Mass storage supports the bulk-only transport with SCSI commands. The
  USB Attached SCSI protocol and the CBI transport of old floppy drives are
  not supported.
- A hub uses its single transaction translator. Multiple translators are
  not selected.
- Keyboard LEDs (caps lock, num lock) are not set.
- The display server opens the input devices at its start. A USB device
  that is connected later, and that is not a reconnected device with a
  reused input device, reaches the console but not the desktop.
- The super speed endpoint companion descriptor is read for bulk
  endpoints only. An interrupt endpoint uses a burst of one packet.
- A controller that addresses only 32 bits is refused.
- Isochronous transfers are not implemented.
