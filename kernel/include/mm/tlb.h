#pragma once
#include <kernel.h>

struct vmspace;

/* TLB maintenance across CPUs. A change to page table entries is followed
 * by a flush on every CPU that may cache the old translation: the local
 * CPU flushes directly, the others receive IRQ_TLB_SHOOTDOWN and flush in
 * the handler. The sender waits for every target to acknowledge. */

void tlb_init(void);

/* Invalidate translations for [va, va + size) in vm on every CPU that may
 * hold them. Callers hold vm->lock. */
void tlb_flush_range(struct vmspace *vm, uintptr_t va, size_t size);
/* Make every other CPU that still has vm loaded switch to the kernel
 * space, so the space's tables can be freed. */
void tlb_drop_vmspace(struct vmspace *vm);
/* Service a shootdown addressed to this CPU, if any. Called from the
 * shootdown interrupt and from spin loops that run with interrupts
 * disabled. Safe to call from anywhere once the kernel runs on struct cpu. */
void tlb_shootdown_poll(void);

struct tlb_stats {
    uint64_t requests;      /* shootdown rounds sent by this kernel */
    uint64_t ipis;          /* target CPUs summed over all rounds */
    uint64_t acks;          /* acknowledgements received */
};
void tlb_get_stats(struct tlb_stats *out);
