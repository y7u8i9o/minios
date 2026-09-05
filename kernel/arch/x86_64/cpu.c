#define KLOG_SUBSYS "cpu"
#include <arch/cpu.h>
#include <debug/panic.h>
#include <klog.h>
#include <lib/string.h>

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

static void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

/* Log the processor the kernel runs on: vendor, family and model, and
 * whether a hypervisor announces itself, so that behaviour that differs
 * between emulation and hardware virtualization can be told apart. */
void cpu_log_identity(void)
{
    uint32_t a, b, c, d;
    char vendor[13];
    cpuid(0, &a, &b, &c, &d);
    memcpy(vendor, &b, 4);
    memcpy(vendor + 4, &d, 4);
    memcpy(vendor + 8, &c, 4);
    vendor[12] = 0;
    uint32_t max_leaf = a;
    cpuid(1, &a, &b, &c, &d);
    unsigned family = (a >> 8) & 0xf, model = (a >> 4) & 0xf, stepping = a & 0xf;
    if (family == 0xf)
        family += (a >> 20) & 0xff;
    if (family == 6 || family >= 0xf)
        model |= ((a >> 16) & 0xf) << 4;
    bool hypervisor = (c >> 31) & 1;
    char hv[13] = "";
    if (hypervisor && max_leaf >= 0x40000000) {
        cpuid(0x40000000, &a, &b, &c, &d);
        memcpy(hv, &b, 4);
        memcpy(hv + 4, &c, 4);
        memcpy(hv + 8, &d, 4);
        hv[12] = 0;
    }
    klog_info("%s family %u model %u stepping %u, %s%s", vendor, family, model, stepping,
              hypervisor ? "hypervisor " : "no hypervisor", hv);
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
