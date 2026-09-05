#include <arch/cpu.h>
#include <debug/panic.h>

extern char boot_stack_top[];

/* One structure per processor, indexed by CPU id. Entry 0 is the boot
 * CPU. The array is filled by cpu_init_boot and smp_park_aps before any
 * other CPU runs and is read only afterwards. */
static struct cpu cpus[MAX_CPUS];

struct cpu *cpu_by_id(unsigned id)
{
    if (id >= MAX_CPUS)
        panic("cpu_by_id: %u out of range", id);
    return &cpus[id];
}

void cpu_init_boot(void)
{
    struct cpu *c = &cpus[0];
    c->self = c;
    c->id = 0;
    c->lapic_id = 0;
    c->current = NULL;
    c->kstack_top = boot_stack_top;
    c->ap_stack_top = boot_stack_top;
    c->cli_depth = 0;
    c->int_enabled = 0;
    c->online = true;
    c->started = true;
    spinlock_init(&c->pmm_cache_lock, "pmm_cpu_cache");
    wrmsr(MSR_GS_BASE, (uint64_t)c);
    wrmsr(MSR_KERNEL_GS_BASE, (uint64_t)c);
}

void push_cli(void)
{
    uint64_t rflags = read_rflags();
    cli();
    struct cpu *c = cpu_current();
    if (c->cli_depth == 0)
        c->int_enabled = (rflags & RFLAGS_IF) != 0;
    c->cli_depth++;
}

void pop_cli(void)
{
    if (read_rflags() & RFLAGS_IF)
        panic("pop_cli with interrupts enabled");
    struct cpu *c = cpu_current();
    if (c->cli_depth <= 0)
        panic("pop_cli without matching push_cli");
    c->cli_depth--;
    if (c->cli_depth == 0 && c->int_enabled)
        sti();
}
