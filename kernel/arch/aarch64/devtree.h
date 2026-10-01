#pragma once
#include <kernel.h>

/* The devices of the machine as the device tree describes them (A7). The
 * structure is filled once by devtree_init, before the bootloader memory
 * that contains the tree is reclaimed, and read only afterwards. Without a
 * tree, or for a missing node, the addresses of QEMU virt apply. */
struct devtree {
    uintptr_t gicd;                 /* GICv3 distributor */
    uintptr_t gicr;                 /* first redistributor region */
    size_t gicr_size;
    uintptr_t its;                  /* GICv3 ITS, 0 if absent */
    uintptr_t ecam;                 /* PCIe configuration space, 0 if absent */
    size_t ecam_size;
    unsigned bus_start, bus_end;    /* buses the ECAM window covers */
    uint32_t msi_rid_base;          /* msi-map: requester IDs from rid_base */
    uint32_t msi_base;              /* translate to ITS device IDs from msi_base */
    uint32_t msi_length;
    uintptr_t rtc;                  /* PL031 */
};

extern struct devtree devtree;

void devtree_init(void);
