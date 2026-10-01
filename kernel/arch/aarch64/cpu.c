#define KLOG_SUBSYS "cpu"
#include <arch/cpu.h>
#include <arch/init.h>
#include <arch/trap.h>
#include <klog.h>

extern char boot_stack_top[];
extern char exception_vectors[];

void cpu_init_boot(void)
{
    struct cpu *c = cpu_by_id(0);
    c->self = c;
    c->id = 0;
    c->current = NULL;
    c->kstack_top = boot_stack_top;
    c->ap_stack_top = boot_stack_top;
    c->cli_depth = 0;
    c->int_enabled = 0;
    c->online = true;
    c->started = true;
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(c->arch.mpidr));
    /* FP and SIMD instructions at EL0 and EL1 (CPACR_EL1.FPEN): user
     * programs use them, and the kernel saves their registers. */
    uint64_t cpacr;
    __asm__ volatile("mrs %0, cpacr_el1" : "=r"(cpacr));
    __asm__ volatile("msr cpacr_el1, %0; isb" : : "r"(cpacr | (3UL << 20)) : "memory");
    spinlock_init(&c->pmm_cache_lock, "pmm_cpu_cache");
    __asm__ volatile("msr tpidr_el1, %0" : : "r"(c) : "memory");
}

void arch_init_cpu_boot(void)
{
    cpu_init_boot();
}

void arch_init_traps(void)
{
    __asm__ volatile("msr vbar_el1, %0; isb" : : "r"(exception_vectors) : "memory");
}

/* The processor and the translation features the port depends on. */
void arch_init_cpu_features(void)
{
    uint64_t midr, mmfr0, mmfr1, pfr0, el;
    __asm__ volatile("mrs %0, midr_el1" : "=r"(midr));
    __asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
    __asm__ volatile("mrs %0, id_aa64mmfr1_el1" : "=r"(mmfr1));
    __asm__ volatile("mrs %0, id_aa64pfr0_el1" : "=r"(pfr0));
    __asm__ volatile("mrs %0, currentel" : "=r"(el));
    static const unsigned pa_bits[] = { 32, 36, 40, 42, 44, 48, 52 };
    unsigned parange = mmfr0 & 0xf;
    unsigned hafdbs = (mmfr1 >> 0) & 0xf;
    klog_info("midr %08lx (implementer %02lx part %03lx), EL%lu, %u bit physical addresses",
              midr, midr >> 24, (midr >> 4) & 0xfff, (el >> 2) & 3,
              parange < 7 ? pa_bits[parange] : 0);
    klog_info("features:%s%s%s%s",
              hafdbs >= 1 ? " access_flag" : "", hafdbs >= 2 ? " dirty_state" : "",
              ((pfr0 >> 16) & 0xf) != 0xf ? " fp" : "", ((pfr0 >> 20) & 0xf) != 0xf ? " asimd" : "");
}
