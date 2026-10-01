#pragma once
#include <kernel.h>

/* The aarch64 part of the per CPU structure, embedded in struct cpu as
 * c->arch. Private to the owning CPU. */
struct arch_cpu {
    uint64_t mpidr;             /* MPIDR_EL1 affinity of the processor */
    /* Last exception taken from EL0 on this CPU, for the panic dump
     * (diagnostics only). */
    uint64_t last_user_esr;
    uint64_t last_user_pc;
    uint64_t last_user_sp;
};
