#define KLOG_SUBSYS "tlb"
#include <mm/tlb.h>
#include <mm/vmm.h>
#include <arch/paging.h>
#include <arch/cpu.h>
#include <arch/smp.h>
#include <arch/irq.h>
#include <sync/spinlock.h>
#include <kassert.h>
#include <klog.h>

enum tlb_kind {
    TLB_FLUSH_RANGE,
    TLB_DROP_VMSPACE,
};

/* The request in flight. tlb_lock serializes senders, so there is at most
 * one round at a time: the sender fills the request, publishes the target
 * mask in pending and waits until every target has cleared its bit. Targets
 * only read the request while their bit is set. */
static DEFINE_SPINLOCK(tlb_lock);
static struct {
    enum tlb_kind kind;
    struct vmspace *vm;
    uintptr_t va;
    size_t size;
} request;
static volatile cpu_mask_t pending;
static struct tlb_stats stats;   /* requests and ipis under tlb_lock, acks atomic */

/* Flush the local TLB for a range of vm, and on an architecture with
 * broadcast invalidation (PAGING_TLB_BROADCAST) the TLBs of every CPU. */
static void flush_local(struct vmspace *vm, uintptr_t va, size_t size)
{
    bool kernel = vm == &kernel_vmspace || va > USER_TOP;
    bool active = kernel || cpu_current()->vm == vm;
    paging_flush_range(vm, kernel, active, va, size);
}

static void service(void)
{
    if (request.kind == TLB_FLUSH_RANGE) {
        flush_local(request.vm, request.va, request.size);
    } else if (cpu_current()->vm == request.vm) {
        vmspace_activate(&kernel_vmspace);
    }
}

void tlb_shootdown_poll(void)
{
    if (!smp_active())
        return;
    cpu_mask_t bit = 1UL << cpu_current()->id;
    /* Acquire: the request written before pending is read after it. */
    if (!(__atomic_load_n(&pending, __ATOMIC_ACQUIRE) & bit))
        return;
    service();
    __atomic_fetch_add(&stats.acks, 1, __ATOMIC_RELAXED);
    __atomic_fetch_and(&pending, ~bit, __ATOMIC_SEQ_CST);
}

static void shootdown_irq(struct trapframe *tf, void *arg)
{
    tlb_shootdown_poll();
}

/* Run one round against targets. The interrupts of the sender are disabled
 * by its locks, and the sender is never one of its own targets, so it
 * waits. A target that spins on a lock that this CPU has acquired services
 * the request from its spin loop. The other targets take the interrupt. */
static void send_round(enum tlb_kind kind, struct vmspace *vm, uintptr_t va, size_t size,
                       cpu_mask_t targets)
{
    spin_lock(&tlb_lock);
    request.kind = kind;
    request.vm = vm;
    request.va = va;
    request.size = size;
    stats.requests++;
    __atomic_store_n(&pending, targets, __ATOMIC_SEQ_CST);
    for (unsigned i = 0; i < smp_cpu_count(); i++) {
        if (targets & (1UL << i)) {
            arch_send_ipi(i, IRQ_TLB_SHOOTDOWN);
            stats.ipis++;
        }
    }
    while (__atomic_load_n(&pending, __ATOMIC_SEQ_CST))
        cpu_relax();
    spin_unlock(&tlb_lock);
}

static cpu_mask_t targets_for(struct vmspace *vm, bool kernel_range)
{
    cpu_mask_t self = 1UL << cpu_current()->id;
    cpu_mask_t m = kernel_range ? smp_online_mask()
                                : __atomic_load_n(&vm->cpu_mask, __ATOMIC_SEQ_CST);
    return m & ~self;
}

void tlb_flush_range(struct vmspace *vm, uintptr_t va, size_t size)
{
    kassert(spin_holding(&vm->lock));
    flush_local(vm, va, size);
    if (!smp_active() || PAGING_TLB_BROADCAST)
        return;
    bool kernel_range = vm == &kernel_vmspace || va > USER_TOP;
    /* The cleared entries before the read of cpu_mask, against the
     * fetch_or and the table walk of vmspace_activate: otherwise a CPU
     * that activates the space meanwhile may walk the old entry and be
     * missing from the mask (A8). */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    cpu_mask_t targets = targets_for(vm, kernel_range);
    if (targets)
        send_round(TLB_FLUSH_RANGE, vm, va, size, targets);
}

void tlb_replace_entry(struct vmspace *vm, pte_t *entry, pte_t value, uintptr_t va, size_t size)
{
    *entry = 0;
    tlb_flush_range(vm, va, size);
    *entry = value;
    paging_publish_entries();
}

void tlb_drop_vmspace(struct vmspace *vm)
{
    if (!smp_active())
        return;
    cpu_mask_t targets = targets_for(vm, false);
    if (targets)
        send_round(TLB_DROP_VMSPACE, vm, 0, 0, targets);
}

void tlb_get_stats(struct tlb_stats *out)
{
    spin_lock(&tlb_lock);
    *out = stats;
    out->acks = __atomic_load_n(&stats.acks, __ATOMIC_RELAXED);
    spin_unlock(&tlb_lock);
}

void tlb_init(void)
{
    irq_register(IRQ_TLB_SHOOTDOWN, shootdown_irq, NULL);
}
