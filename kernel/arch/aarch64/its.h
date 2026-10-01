#pragma once
#include <kernel.h>
#include <arch/irq.h>

/* LPIs and the GICv3 Interrupt Translation Service (A7). An MSI write to
 * GITS_TRANSLATER contains an event ID. The ITS translates the pair of the
 * writer's device ID and the event ID into an LPI for a collection, which
 * names the redistributor that receives it. */

#define LPI_BASE 8192
#define LPI_MAX  256                    /* LPIs irq_alloc hands out */

/* Set up the LPI tables of the redistributor at gicr (physical address
 * gicr_phys) and the ITS, if the device tree lists one. Called by
 * arch_init_interrupts on the boot CPU. */
void its_init(volatile uint8_t *gicr, uintptr_t gicr_phys);
/* The LPI tables and the ITS collection of an application processor
 * (A8): an MSI composed on a CPU is delivered to that CPU. */
void its_init_cpu(volatile uint8_t *gicr, uintptr_t gicr_phys);
int its_alloc(void);
void its_register(unsigned lpi, irq_handler_fn fn, void *arg);
void its_dispatch(struct trapframe *tf, unsigned lpi);
/* Map the next event of devid to the LPI irq in the collection of cpu and
 * return the MSI address and data that raise it. */
void its_msi_compose(uint32_t devid, unsigned irq, unsigned cpu, uint64_t *addr, uint32_t *data);
