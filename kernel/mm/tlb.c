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

/* Flush the local TLB for a range of vm. */
static void flush_local(struct vmspace *vm, uintptr_t va, size_t size)
{
    struct cpu *c = cpu_current();
    bool active = vm == &kernel_vmspace || va > USER_TOP || c->vm == vm;
    if (!active)
        return;
    if (va > USER_TOP || size <= 64 * PAGE_SIZE) {
        for (size_t off = 0; off < size; off += PAGE_SIZE)
            paging_invlpg(va + off);
    } else {
        paging_load(read_cr3());
    }
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
    if (!(pending & bit))
        return;
    service();
    __atomic_fetch_add(&stats.acks, 1, __ATOMIC_RELAXED);
    __atomic_fetch_and(&pending, ~bit, __ATOMIC_SEQ_CST);
}

static void shootdown_irq(struct trapframe *tf, void *arg)
{
    tlb_shootdown_poll();
}

/* Run one round against targets. The sender's interrupts are disabled by
 * its locks and it is never a target itself, so it simply waits; any
 * target that spins on a lock this CPU holds services the request from
 * its spin loop, the others take the interrupt. */
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
    if (!smp_active())
        return;
    bool kernel_range = vm == &kernel_vmspace || va > USER_TOP;
    cpu_mask_t targets = targets_for(vm, kernel_range);
    if (targets)
        send_round(TLB_FLUSH_RANGE, vm, va, size, targets);
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
