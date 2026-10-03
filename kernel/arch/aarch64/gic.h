#pragma once

/* Set up the redistributor and the CPU interface of an application
 * processor (A8). */
void gic_init_cpu(void);
/* The MSI address and data that raise the shared interrupt irq of the
 * GICv2m frame on a GICv2. */
void gic_v2m_msi_compose(unsigned irq, uint64_t *addr, uint32_t *data);
