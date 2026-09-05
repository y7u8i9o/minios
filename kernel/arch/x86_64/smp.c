#define KLOG_SUBSYS "smp"
#include <arch/smp.h>
#include <arch/cpu.h>
#include <arch/apic.h>
#include <arch/gdt.h>
#include <arch/trap.h>
#include <arch/paging.h>
#include <arch/syscall.h>
#include <arch/irq.h>
#include <limine.h>
#include <mm/vmm.h>
#include <mm/tlb.h>
#include <drivers/timer.h>
#include <sched/sched.h>
#include <klog.h>
#include <debug/panic.h>

__used __section(".limine_requests")
static volatile struct limine_mp_request mp_request = {
    .id = LIMINE_MP_REQUEST_ID, .revision = 0, .response = NULL, .flags = 0,
};

/* Written by smp_park_aps on the boot CPU before any AP runs kernel code,
 * read only afterwards. */
static unsigned cpu_count = 1;
static volatile bool aps_released;
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
        if (cpu_by_id(i)->online)
            m |= 1UL << i;
    }
    return m;
}

/* Second stage of an AP, on its kernel stack with the kernel page tables
 * active. The GDT and IDT are loaded here so nothing references the
 * bootloader's tables once its memory is reclaimed. */
static __noreturn void ap_main(struct cpu *c)
{
    gdt_init_cpu(c->id, (uintptr_t)c->kstack_top);
    /* Loading the segment registers cleared the GS base set in ap_entry. */
    wrmsr(MSR_GS_BASE, (uint64_t)c);
    wrmsr(MSR_KERNEL_GS_BASE, (uint64_t)c);
    idt_load();
    __atomic_store_n(&c->online, true, __ATOMIC_SEQ_CST);
    while (!aps_released)
        cpu_relax();

    paging_enable_features();
    lapic_init();
    syscall_init_cpu();
    timer_init_cpu();
    sched_init_cpu();
    __atomic_store_n(&c->started, true, __ATOMIC_SEQ_CST);
    klog_info("cpu %u online, lapic id %u", c->id, c->lapic_id);
    sti();
    sched_idle_loop();
}

/* Entry from the bootloader, on its stack and page tables with interrupts
 * disabled. Switch to the kernel's page tables and stack, then continue in
 * ap_main. */
static __noreturn void ap_entry(struct limine_mp_info *info)
{
    struct cpu *c = (struct cpu *)info->extra_argument;
    paging_load(kernel_vmspace.pml4_phys);
    wrmsr(MSR_GS_BASE, (uint64_t)c);
    wrmsr(MSR_KERNEL_GS_BASE, (uint64_t)c);
    c->vm = &kernel_vmspace;
    __asm__ volatile(
        "movq %0, %%rsp\n"
        "xorl %%ebp, %%ebp\n"
        "movq %1, %%rdi\n"
        "call ap_main_trampoline\n"
        : : "r"(c->kstack_top), "r"(c) : "memory");
    __builtin_unreachable();
}

/* Reached only through the stack switch above; keeps ap_main off the
 * bootloader stack entirely. */
__noreturn void ap_main_trampoline(struct cpu *c);
__noreturn void ap_main_trampoline(struct cpu *c)
{
    ap_main(c);
}

void smp_park_aps(void)
{
    struct limine_mp_response *mp = mp_request.response;
    if (!mp || mp->cpu_count <= 1) {
        klog_info("single processor");
        return;
    }
    struct cpu *bsp = cpu_by_id(0);
    bsp->lapic_id = mp->bsp_lapic_id;
    unsigned id = 1;
    for (uint64_t i = 0; i < mp->cpu_count && id < MAX_CPUS; i++) {
        struct limine_mp_info *info = mp->cpus[i];
        if (info->lapic_id == mp->bsp_lapic_id)
            continue;
        struct cpu *c = cpu_by_id(id);
        c->self = c;
        c->id = id;
        c->lapic_id = info->lapic_id;
        c->vm = &kernel_vmspace;
        spinlock_init(&c->pmm_cache_lock, "pmm_cpu_cache");
        c->kstack_top = kstack_alloc();
        if (!c->kstack_top)
            panic("smp: no stack for cpu %u", id);
        c->ap_stack_top = c->kstack_top;
        info->extra_argument = (uint64_t)c;
        __atomic_store_n(&info->goto_address, ap_entry, __ATOMIC_SEQ_CST);
        while (!__atomic_load_n(&c->online, __ATOMIC_SEQ_CST))
            cpu_relax();
        id++;
    }
    if (mp->cpu_count > MAX_CPUS)
        klog_warn("only %u of %lu processors are used", MAX_CPUS, mp->cpu_count);
    cpu_count = id;
    klog_info("%u processors parked", cpu_count);
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
    __atomic_store_n(&aps_released, true, __ATOMIC_SEQ_CST);
    for (unsigned i = 1; i < cpu_count; i++) {
        while (!__atomic_load_n(&cpu_by_id(i)->started, __ATOMIC_SEQ_CST))
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
            lapic_send_ipi(c->lapic_id, IRQ_HALT);
    }
}
