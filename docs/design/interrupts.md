# Interrupts and timing (M6)

## Local APIC

`arch/x86_64/apic.c` maps the local APIC (base from `IA32_APIC_BASE`) with
`vmm_map_mmio`, masks the LVT entries, sets the spurious vector to `0xff`
and enables it through the spurious vector register. The legacy PICs, which
`idt_init` had remapped to vectors 32 to 47, are masked completely.

## I/O APIC

The I/O APIC at its default address `0xfec00000` is mapped the same way.
All redirection entries start masked. `ioapic_route(gsi, vector, masked)`
programs fixed delivery to the boot CPU, edge triggered, active high. The
PS/2 keyboard is GSI 1 (vector 33). The PIT would be GSI 2 (vector 34) but
is only used in polled mode for calibration and is left masked.

ACPI tables are not parsed. QEMU's q35 machine uses the default addresses
and the ISA identity mapping, which is all the kernel targets.

## Timer

`lapic_timer_init` measures the APIC timer: with the divider at 16 it
counts down from `0xffffffff` while `pit_wait_us(10000)` busy waits on PIT
channel 2 in one shot mode (gate through port `0x61`). The elapsed count
times 100 gives ticks per second, and the timer is then programmed periodic
at `TIMER_HZ` (1000). Under QEMU TCG this yields about 63 million ticks per
second.

`drivers/timer.c` registers the vector 32 handler, counts the interrupts
of the boot CPU and calls the scheduler hook installed with
`timer_set_tick_handler`. Time itself does not come from that count:
`timer_ms` and `timer_ticks` read the TSC (`arch_clock_read`), which
`timer_early_init` calibrates against the PIT over 20 ms
(`arch_clock_calibrate` in `arch/x86_64/clock.c`) as the first statement
of `kmain`,
so that the log can stamp every line with the time since the kernel
entry. Under TCG the emulator delivers fewer than 1000 timer interrupts
per second, and a clock built on the interrupt count ran about 25
percent slow, while the TSC follows host time. `sleep_ms`
blocks until the TSC based time reaches the deadline, and the timer
interrupt only provides the moments at which sleepers are examined.

## Interrupt dispatch

`arch/x86_64/irq.c` keeps a handler table indexed by vector. `trap_dispatch`
routes vectors 32 and above to `irq_dispatch`, which calls the handler with
interrupts disabled and sends the EOI afterwards. Spurious interrupts get no
EOI. Handlers are registered before their line is unmasked, so the table
needs no lock.

## PS/2 keyboard

`arch/x86_64/ps2kbd.c` reads scancode set 1 from port `0x60`. Since M47 it
reports key codes to the input core (`input.md`), which owns the state
described below (this section is the M6 design). Shift, control,
alt and caps lock are tracked, `0xe0` prefixed keys are ignored for now.
Control combined with a letter yields the corresponding control character.
The line discipline echoes characters, handles backspace and control U, and
moves a line into the ready ring on newline. `ps2kbd_getc` returns the next
character of a completed line or `-1`. All state is behind `kbd_lock`,
which the interrupt handler takes as well. `ps2kbd_feed_scancode` is the
entry point shared by the handler and the tests.

## Tests

`tests/cases/timer` checks that interrupts are enabled, that `sleep_ms(50)`
lasts at least 50 ticks and that ten 10 ms sleeps take between 100 and 1000
ticks. `tests/cases/kbd` feeds scancode sequences with shift, backspace,
control, caps lock and an extended prefix and checks the resulting lines.
