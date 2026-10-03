#pragma once
#include <kernel.h>

/* The x86_64 part of the per CPU structure, embedded in struct cpu as
 * c->arch. Private to the owning CPU. user_rsp must remain the first field:
 * syscall.S reaches it at a fixed offset of struct cpu. */
struct arch_cpu {
    uint64_t user_rsp;          /* scratch for the syscall entry */
    uint32_t lapic_id;
    /* Last trap or interrupt taken from user mode on this CPU, as the CPU
     * pushed it, retained for the panic dump (diagnostics only). */
    uint64_t last_user_vector;
    uint64_t last_user_error;
    uint64_t last_user_rip;
    uint64_t last_user_cs;
    uint64_t last_user_rsp;
    uint64_t last_user_ss;
    uint64_t last_user_cr3;
    void *last_user_frame;
};
