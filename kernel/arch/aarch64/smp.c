#define KLOG_SUBSYS "smp"
#include <arch/smp.h>
#include <arch/cpu.h>
#include <arch/init.h>
#include <arch/irq.h>
#include <arch/paging.h>
#include <limine.h>
#include <mm/vmm.h>
#include <drivers/timer.h>
#include <sched/sched.h>
#include <klog.h>
#include <debug/panic.h>
#include "gic.h"

/* Application processors through the Limine MP protocol (A8). Limine
 * starts every processor with PSCI and parks it on its own stack and
 * translation tables until the kernel writes goto_address. The kernel
 * releases one processor at a time into ap_entry (start.S) with the
 * translation state and the stack in ap_boot. */

__used __section(".limine_requests")
static volatile struct limine_mp_request mp_request = {
    .id = LIMINE_MP_REQUEST_ID, .revision = 0, .response = NULL, .flags = 0,
};

/* Read by ap_entry: MAIR_EL1, TCR_EL1, TTBR1_EL1, TTBR0_EL1, the stack top
 * and struct cpu of the processor being released. Written by the boot CPU
 * before each release, which waits until the processor is online before
 * it writes the next. */
struct ap_boot {
    uint64_t mair, tcr, ttbr1, ttbr0;
    uint64_t stack;
    struct cpu *cpu;
};
struct ap_boot ap_boot;

/* Written by smp_park_aps on the boot CPU before any AP runs kernel code,
 * read only afterwards. */
static unsigned cpu_count = 1;
static bool aps_released;               /* stored with release, loaded with acquire */
static bool active;

unsigned smp_cpu_count(void)
{
    return cpu_count;
}

bool smp_active(void)
{
    return active;
}

cpu_mask_t smp_online_mask(void)
{
    cpu_mask_t m = 0;
    for (unsigned i = 0; i < cpu_count; i++) {
        if (__atomic_load_n(&cpu_by_id(i)->online, __ATOMIC_ACQUIRE))
            m |= 1UL << i;
    }
    return m;
}

__noreturn void ap_main(struct cpu *c);
__noreturn void ap_main(struct cpu *c)
{
    __asm__ volatile("msr tpidr_el1, %0" : : "r"(c) : "memory");
    __asm__ volatile("mrs %0, midr_el1" : "=r"(c->arch.midr));
    arch_init_traps();
    cpu_init_el0_access();
    __atomic_store_n(&c->online, true, __ATOMIC_RELEASE);
    while (!__atomic_load_n(&aps_released, __ATOMIC_ACQUIRE))
        cpu_relax();

    gic_init_cpu();
    timer_init_cpu();
    sched_init_cpu();
    __atomic_store_n(&c->started, true, __ATOMIC_RELEASE);
    klog_info("cpu %u online, mpidr %lx", c->id, c->arch.mpidr);
    arch_irq_enable();
    sched_idle_loop();
}

void ap_entry(struct limine_mp_info *info);

void smp_park_aps(void)
{
    struct limine_mp_response *mp = mp_request.response;
    if (!mp || mp->cpu_count <= 1) {
        klog_info("1 processor, no application processors");
        return;
    }
    struct cpu *bsp = cpu_by_id(0);
    paging_cpu_state(&ap_boot.mair, &ap_boot.tcr, &ap_boot.ttbr1, &ap_boot.ttbr0);
    unsigned id = 1;
    for (uint64_t i = 0; i < mp->cpu_count && id < MAX_CPUS; i++) {
        struct limine_mp_info *info = mp->cpus[i];
        if (info->mpidr == mp->bsp_mpidr)
            continue;
        struct cpu *c = cpu_by_id(id);
        c->self = c;
        c->id = id;
        c->arch.mpidr = info->mpidr;
        c->vm = &kernel_vmspace;
        spinlock_init(&c->pmm_cache_lock, "pmm_cpu_cache");
        c->kstack_top = kstack_alloc();
        if (!c->kstack_top)
            panic("smp: no stack for cpu %u", id);
        c->ap_stack_top = c->kstack_top;
        ap_boot.stack = (uint64_t)c->kstack_top;
        ap_boot.cpu = c;
        /* The release store orders the stores to ap_boot and c before
         * goto_address. The caches of the processor are coherent with the
         * caches of the boot CPU. */
        __atomic_store_n(&info->goto_address, ap_entry, __ATOMIC_RELEASE);
        while (!__atomic_load_n(&c->online, __ATOMIC_ACQUIRE))
            cpu_relax();
        id++;
    }
    if (mp->cpu_count > MAX_CPUS)
        klog_warn("only %u of %lu processors are used", MAX_CPUS, mp->cpu_count);
    cpu_count = id;
    klog_info("%u processors parked on kernel stacks, bsp mpidr %lx", cpu_count, bsp->arch.mpidr);
}

static void halt_irq(struct trapframe *tf, void *arg)
{
    cpu_halt_forever();
}

void smp_start_aps(void)
{
    if (cpu_count <= 1)
        return;
    irq_register(IRQ_HALT, halt_irq, NULL);
    active = true;
    __atomic_store_n(&aps_released, true, __ATOMIC_RELEASE);
    for (unsigned i = 1; i < cpu_count; i++) {
        while (!__atomic_load_n(&cpu_by_id(i)->started, __ATOMIC_ACQUIRE))
            cpu_relax();
    }
    klog_info("%u processors running", cpu_count);
}

void smp_halt_others(void)
{
    if (!active)
        return;
    struct cpu *self = cpu_current();
    for (unsigned i = 0; i < cpu_count; i++) {
        struct cpu *c = cpu_by_id(i);
        if (c != self && c->online)
            arch_send_ipi(i, IRQ_HALT);
    }
}
