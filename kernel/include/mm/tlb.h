#pragma once
#include <kernel.h>
#include <arch/paging.h>

struct vmspace;

/* TLB maintenance across CPUs. A change to page table entries is followed
 * by a flush on every CPU that may cache the old translation: the local
 * CPU flushes directly, the others receive IRQ_TLB_SHOOTDOWN and flush in
 * the handler. The sender waits for every target to acknowledge. Where
 * the architecture invalidates the TLBs of every CPU with one instruction
 * (PAGING_TLB_BROADCAST, aarch64), a range needs no interrupts. */

void tlb_init(void);

/* Invalidate translations for [va, va + size) in vm on every CPU whose TLB
 * may contain them. The caller has acquired vm->lock. */
void tlb_flush_range(struct vmspace *vm, uintptr_t va, size_t size);
/* Replace the present entry at entry, which maps [va, va + size) of vm,
 * with value, where value maps another frame or has another size (a block
 * split into a table): break before make. The entry is cleared and the
 * range flushed on every CPU before value is written, as ARMv8 requires.
 * A CPU that accesses the range meanwhile faults and waits for vm->lock.
 * The caller has acquired vm->lock. */
void tlb_replace_entry(struct vmspace *vm, pte_t *entry, pte_t value, uintptr_t va, size_t size);
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
