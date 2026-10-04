#pragma once

/* Set up the redistributor and the CPU interface of an application
 * processor (A8). */
void gic_init_cpu(void);
/* The MSI address and data that raise the shared interrupt irq of the
 * GICv2m frame on a GICv2. */
struct devinfo;
/* The interrupt controller properties of the platform node of /dev/devices. */
void gic_describe(struct devinfo *d);
void gic_v2m_msi_compose(unsigned irq, uint64_t *addr, uint32_t *data);
