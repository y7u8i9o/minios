#include <cpu.h>
#include <arch/cpu.h>
#include <debug/panic.h>

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

void push_cli(void)
{
    unsigned long flags = arch_irq_save();
    struct cpu *c = cpu_current();
    if (c->cli_depth == 0)
        c->int_enabled = arch_irq_flags_enabled(flags);
    c->cli_depth++;
}

void pop_cli(void)
{
    if (arch_irqs_enabled())
        panic("pop_cli with interrupts enabled");
    struct cpu *c = cpu_current();
    if (c->cli_depth <= 0)
        panic("pop_cli without matching push_cli");
    c->cli_depth--;
    if (c->cli_depth == 0 && c->int_enabled)
        arch_irq_enable();
}
