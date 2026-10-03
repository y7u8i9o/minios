#pragma once
#include <kernel.h>

/* The aarch64 part of the per CPU structure, embedded in struct cpu as
 * c->arch. Private to the owning CPU. */
struct arch_cpu {
    uint64_t mpidr;             /* MPIDR_EL1 affinity of the processor */
    volatile uint8_t *gicr;     /* its GICv3 redistributor (gic.c) */
    uint8_t gic_mask;           /* GICv2: its CPU interface as a target bit (gic.c) */
    uint64_t asid_active;       /* tag of the space loaded in TTBR0, 0 for none, asid_lock */
    uint64_t asid_reserved;     /* tag retained across a rollover until the next switch, asid_lock */
    uint64_t its_rdbase;        /* its redistributor as ITS commands name it, set once (its.c) */
    /* Last exception taken from EL0 on this CPU, for the panic dump
     * (diagnostics only). */
    uint64_t last_user_esr;
    uint64_t last_user_pc;
    uint64_t last_user_sp;
};
