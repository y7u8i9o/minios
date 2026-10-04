# USB: xHCI, enumeration and HID

Since D2 of `docs/plan/drivers.md`, minios drives xHCI host controllers and
the USB keyboards, mice and tablets connected to them. These devices report
to the input core and appear as `/dev/input/eventN`, like the PS/2 and
virtio input devices. The code is generic and runs on both architectures.

| File | Content |
|---|---|
| `kernel/drivers/usb/xhci.c` | the xHCI controller driver and the controller thread |
| `kernel/drivers/usb/usb.c` | the USB core: descriptors, configuration, binding |
| `kernel/drivers/usb/hid.c` | the HID class driver and the report descriptor parser |
| `kernel/drivers/usb/usb.h` | the declarations shared by the three files |
| `kernel/include/drivers/usb.h` | `usb_init` and `usb_device_count` |

The driver is written from the eXtensible Host Controller Interface
specification 1.2, the USB 2.0 specification (chapter 9), the Device Class
Definition for HID 1.11 and the HID Usage Tables 1.4. No existing USB stack
was used. TinyUSB has no xHCI driver, and CherryUSB publishes its xHCI driver
only as a compiled library.

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
interface. The log line is
`port 5: QEMU USB Keyboard, 0627:0001, high speed, slot 1, 1 interface`.

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

## Limits

- USB hubs are not supported. A device behind a hub is not enumerated.
  Real computers often place hubs between the root ports and the devices
  (D5 of the plan).
- Only HID interfaces are bound. USB mass storage is D5.
- Keyboard LEDs (caps lock, num lock) are not set.
- The display server opens the input devices at its start. A USB device
  that is connected later, and that is not a reconnected device with a
  reused input device, reaches the console but not the desktop.
- The super speed endpoint companion descriptor is not read. An interrupt
  endpoint uses a burst of one packet.
- A controller that addresses only 32 bits is refused.
- Isochronous and bulk transfers are not implemented.
