#define KLOG_SUBSYS "cpu"
#include <arch/cpu.h>
#include <debug/panic.h>
#include <klog.h>
#include <lib/string.h>

extern char boot_stack_top[];

void cpu_init_boot(void)
{
    struct cpu *c = cpu_by_id(0);
    c->self = c;
    c->id = 0;
    c->arch.lapic_id = 0;
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

struct cpu_features cpu_features;

static void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(0));
}

void cpu_identify(void)
{
    struct cpu_features *f = &cpu_features;
    uint32_t a, b, c, d;
    cpuid(0, &a, &b, &c, &d);
    memcpy(f->vendor, &b, 4);
    memcpy(f->vendor + 4, &d, 4);
    memcpy(f->vendor + 8, &c, 4);
    f->vendor[12] = 0;
    f->intel = strcmp(f->vendor, "GenuineIntel") == 0;
    f->amd = strcmp(f->vendor, "AuthenticAMD") == 0;

    cpuid(1, &a, &b, &c, &d);
    f->family = (a >> 8) & 0xf;
    f->model = (a >> 4) & 0xf;
    f->stepping = a & 0xf;
    if (f->family == 0xf)
        f->family += (a >> 20) & 0xff;
    if (f->family == 6 || f->family >= 0xf)
        f->model |= ((a >> 16) & 0xf) << 4;
    f->pge = (d >> 13) & 1;
    f->pat = (d >> 16) & 1;
    f->fxsr = (d >> 24) & 1;
    f->sse2 = (d >> 26) & 1;
    f->pcid = (c >> 17) & 1;
    f->x2apic = (c >> 21) & 1;
    f->tsc_deadline = (c >> 24) & 1;
    /* The hypervisor leaf is separate from the basic range; its eax is
     * the maximum hypervisor leaf, which is at least 0x40000000. */
    bool hypervisor = (c >> 31) & 1;
    if (hypervisor) {
        cpuid(0x40000000, &a, &b, &c, &d);
        if (a < 0x40000000)
            b = c = d = 0;
        memcpy(f->hypervisor, &b, 4);
        memcpy(f->hypervisor + 4, &c, 4);
        memcpy(f->hypervisor + 8, &d, 4);
        f->hypervisor[12] = 0;
    }

    cpuid(0x80000000, &a, &b, &c, &d);
    uint32_t max_ext = a;
    f->phys_bits = 36;
    if (max_ext >= 0x80000001) {
        cpuid(0x80000001, &a, &b, &c, &d);
        f->nx = (d >> 20) & 1;
        f->pdpe1gb = (d >> 26) & 1;
    }
    if (max_ext >= 0x80000007) {
        cpuid(0x80000007, &a, &b, &c, &d);
        f->invariant_tsc = (d >> 8) & 1;
    }
    if (max_ext >= 0x80000008) {
        cpuid(0x80000008, &a, &b, &c, &d);
        f->phys_bits = a & 0xff;
    }
    char brand[49] = "";
    if (max_ext >= 0x80000004) {
        uint32_t *w = (uint32_t *)brand;
        for (uint32_t leaf = 0; leaf < 3; leaf++)
            cpuid(0x80000002 + leaf, &w[leaf * 4], &w[leaf * 4 + 1], &w[leaf * 4 + 2], &w[leaf * 4 + 3]);
        brand[48] = '\0';
    }
    const char *name = brand;
    while (*name == ' ')
        name++;
    klog_info("%s family %u model %u stepping %u \"%s\", %s%s", f->vendor, f->family, f->model,
              f->stepping, name, hypervisor ? "hypervisor " : "no hypervisor", f->hypervisor);
    klog_info("features:%s%s%s%s%s%s%s%s%s%s, %u physical address bits",
              f->nx ? " nx" : "", f->pge ? " pge" : "", f->pat ? " pat" : "",
              f->fxsr ? " fxsr" : "", f->sse2 ? " sse2" : "", f->x2apic ? " x2apic" : "",
              f->pcid ? " pcid" : "", f->pdpe1gb ? " pdpe1gb" : "",
              f->invariant_tsc ? " invariant_tsc" : "", f->tsc_deadline ? " tsc_deadline" : "",
              f->phys_bits);
}
