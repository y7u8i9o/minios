#pragma once
#include <kernel.h>

/* Interrupt vectors used by the kernel. */
#define IRQ_VECTOR_BASE   32
#define IRQ_TIMER         32
#define IRQ_KEYBOARD      33
#define IRQ_PIT           34
#define IRQ_MOUSE         35
/* Inter processor interrupts. */
#define IRQ_TLB_SHOOTDOWN 0xf0
#define IRQ_HALT          0xf1
#define IRQ_SPURIOUS      0xff

/* Legacy IRQ lines as they appear on the I/O APIC (ISA identity mapping,
 * except IRQ 0 which the PIT drives through GSI 2 on q35). */
#define GSI_PIT       2
#define GSI_KEYBOARD  1
#define GSI_MOUSE     12

/* Enable and configure the local APIC of the calling CPU. Maps the
 * register page on the first call. */
void lapic_init(void);
uint32_t lapic_id(void);
void lapic_eoi(void);
/* Measure the local APIC timer frequency with the PIT. Boot CPU only,
 * before any timer is started. Returns the measured ticks per second. */
uint64_t lapic_timer_calibrate(void);
/* Program the calling CPU's timer for periodic interrupts on IRQ_TIMER at
 * hz, using the calibration of the boot CPU. */
void lapic_timer_start(unsigned hz);
/* Send a fixed vector interrupt to the CPU with the given APIC id. Waits
 * until the previous IPI from this CPU has been delivered. */
void lapic_send_ipi(uint32_t lapic_id, uint8_t vector);

void ioapic_init(void);
void ioapic_route(unsigned gsi, uint8_t vector, bool masked);
void ioapic_mask(unsigned gsi, bool masked);
