#define KLOG_SUBSYS "cpu"
#include <arch/cpu.h>
#include <debug/panic.h>
#include <klog.h>
#include <lib/string.h>
#include <lib/printf.h>
#include <drivers/devinfo.h>
#include <drivers/timer.h>
#include <arch/smp.h>
#include <cpu.h>

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

static void cpuid_sub(uint32_t leaf, uint32_t sub, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf), "c"(sub));
}

static void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    cpuid_sub(leaf, 0, a, b, c, d);
}

/* The caches from the deterministic cache parameters: leaf 4 on Intel,
 * leaf 0x8000001d on AMD with topology extensions. Both have the same
 * format. */
static void read_cache_leaf(struct cpu_features *f, uint32_t leaf)
{
    for (uint32_t sub = 0; sub < 16 && f->ncaches < 8; sub++) {
        uint32_t a, b, c, d;
        cpuid_sub(leaf, sub, &a, &b, &c, &d);
        unsigned type = a & 0x1f;
        if (type == 0)
            break;
        struct cpu_cache *k = &f->caches[f->ncaches++];
        k->level = (uint8_t)((a >> 5) & 7);
        k->type = type == 1 ? 'd' : type == 2 ? 'i' : 'u';
        k->shared = (uint16_t)(((a >> 14) & 0xfff) + 1);
        k->ways = (uint16_t)(((b >> 22) & 0x3ff) + 1);
        k->line = (uint16_t)((b & 0xfff) + 1);
        unsigned partitions = ((b >> 12) & 0x3ff) + 1;
        k->sets = c + 1;
        k->size = (uint32_t)k->ways * partitions * k->line * k->sets;
    }
}

/* The caches from the older AMD leaves: L1 in 0x80000005, L2 and L3 in
 * 0x80000006. The associativity of L2 and L3 is an encoded value. */
static void read_cache_amd_legacy(struct cpu_features *f)
{
    static const uint16_t assoc[16] = { 0, 1, 2, 3, 4, 5, 6, 8, 0, 16, 0, 32, 48, 64, 96, 128 };
    uint32_t a, b, c, d;
    if (f->max_ext_leaf >= 0x80000005) {
        cpuid(0x80000005, &a, &b, &c, &d);
        if (c >> 24)
            f->caches[f->ncaches++] = (struct cpu_cache){ 1, 'd', (uint16_t)((c >> 16) & 0xff), (uint16_t)(c & 0xff),
                                                          0, 0, (c >> 24) * 1024 };
        if (d >> 24)
            f->caches[f->ncaches++] = (struct cpu_cache){ 1, 'i', (uint16_t)((d >> 16) & 0xff), (uint16_t)(d & 0xff),
                                                          0, 0, (d >> 24) * 1024 };
    }
    if (f->max_ext_leaf >= 0x80000006) {
        cpuid(0x80000006, &a, &b, &c, &d);
        if (c >> 16)
            f->caches[f->ncaches++] = (struct cpu_cache){ 2, 'u', assoc[(c >> 12) & 0xf], (uint16_t)(c & 0xff),
                                                          0, 0, (c >> 16) * 1024 };
        if (d >> 18)
            f->caches[f->ncaches++] = (struct cpu_cache){ 3, 'u', assoc[(d >> 12) & 0xf], (uint16_t)(d & 0xff),
                                                          0, 0, (d >> 18) * 512 * 1024 };
    }
}

void cpu_identify(void)
{
    struct cpu_features *f = &cpu_features;
    uint32_t a, b, c, d;
    cpuid(0, &a, &b, &c, &d);
    f->max_leaf = a;
    memcpy(f->vendor, &b, 4);
    memcpy(f->vendor + 4, &d, 4);
    memcpy(f->vendor + 8, &c, 4);
    f->vendor[12] = 0;
    f->intel = strcmp(f->vendor, "GenuineIntel") == 0;
    f->amd = strcmp(f->vendor, "AuthenticAMD") == 0;

    cpuid(1, &a, &b, &c, &d);
    f->signature = a;
    f->leaf1_ecx = c;
    f->leaf1_edx = d;
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

    if (f->max_leaf >= 7) {
        cpuid(7, &a, &b, &c, &d);
        f->leaf7_ebx = b;
        f->leaf7_ecx = c;
        f->leaf7_edx = d;
    }

    cpuid(0x80000000, &a, &b, &c, &d);
    uint32_t max_ext = a;
    f->max_ext_leaf = a;
    f->phys_bits = 36;
    f->virt_bits = 48;
    if (max_ext >= 0x80000001) {
        cpuid(0x80000001, &a, &b, &c, &d);
        f->ext1_ecx = c;
        f->ext1_edx = d;
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
        f->virt_bits = (a >> 8) & 0xff;
    }
    if (f->max_leaf >= 4 && !f->amd)
        read_cache_leaf(f, 4);
    else if (f->amd && max_ext >= 0x8000001d && (f->ext1_ecx & (1u << 22)))
        read_cache_leaf(f, 0x8000001d);
    if (!f->ncaches)
        read_cache_amd_legacy(f);
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
    strlcpy(f->brand, name, sizeof f->brand);
    klog_info("%s family %u model %u stepping %u \"%s\", %s%s", f->vendor, f->family, f->model,
              f->stepping, name, hypervisor ? "hypervisor " : "no hypervisor", f->hypervisor);
    klog_info("features:%s%s%s%s%s%s%s%s%s%s, %u physical address bits",
              f->nx ? " nx" : "", f->pge ? " pge" : "", f->pat ? " pat" : "",
              f->fxsr ? " fxsr" : "", f->sse2 ? " sse2" : "", f->x2apic ? " x2apic" : "",
              f->pcid ? " pcid" : "", f->pdpe1gb ? " pdpe1gb" : "",
              f->invariant_tsc ? " invariant_tsc" : "", f->tsc_deadline ? " tsc_deadline" : "",
              f->phys_bits);
}

/* The names of the feature bits (Intel SDM volume 2, CPUID; AMD APM
 * volume 3, appendix E), indexed by bit. Reserved bits have no name. */
static const char *const leaf1_edx_names[32] = {
    "fpu", "vme", "de", "pse", "tsc", "msr", "pae", "mce", "cx8", "apic", NULL, "sep", "mtrr", "pge", "mca",
    "cmov", "pat", "pse36", "psn", "clflush", NULL, "ds", "acpi", "mmx", "fxsr", "sse", "sse2", "ss", "htt",
    "tm", NULL, "pbe",
};
static const char *const leaf1_ecx_names[32] = {
    "sse3", "pclmulqdq", "dtes64", "monitor", "ds_cpl", "vmx", "smx", "est", "tm2", "ssse3", "cnxt_id", "sdbg",
    "fma", "cx16", "xtpr", "pdcm", NULL, "pcid", "dca", "sse4_1", "sse4_2", "x2apic", "movbe", "popcnt",
    "tsc_deadline", "aes", "xsave", "osxsave", "avx", "f16c", "rdrand", "hypervisor",
};
static const char *const leaf7_ebx_names[32] = {
    "fsgsbase", "tsc_adjust", "sgx", "bmi1", "hle", "avx2", NULL, "smep", "bmi2", "erms", "invpcid", "rtm",
    NULL, NULL, "mpx", NULL, "avx512f", "avx512dq", "rdseed", "adx", "smap", "avx512ifma", NULL, "clflushopt",
    "clwb", NULL, "avx512pf", "avx512er", "avx512cd", "sha_ni", "avx512bw", "avx512vl",
};
static const char *const leaf7_ecx_names[32] = {
    "prefetchwt1", "avx512vbmi", "umip", "pku", "ospke", "waitpkg", "avx512_vbmi2", "cet_ss", "gfni", "vaes",
    "vpclmulqdq", "avx512_vnni", "avx512_bitalg", NULL, "avx512_vpopcntdq", NULL, "la57", NULL, NULL, NULL,
    NULL, NULL, "rdpid", NULL, NULL, "cldemote", NULL, "movdiri", "movdir64b", NULL, "sgx_lc", NULL,
};
static const char *const leaf7_edx_names[32] = {
    NULL, NULL, "avx512_4vnniw", "avx512_4fmaps", "fsrm", NULL, NULL, NULL, "avx512_vp2intersect", NULL,
    "md_clear", NULL, NULL, NULL, "serialize", "hybrid", "tsxldtrk", NULL, "pconfig", NULL, "cet_ibt", NULL,
    "amx_bf16", "avx512_fp16", "amx_tile", "amx_int8", "spec_ctrl", "stibp", "flush_l1d", "arch_capabilities",
    NULL, "ssbd",
};
static const char *const ext1_edx_names[32] = {
    NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, "syscall", NULL, NULL, NULL, NULL,
    NULL, NULL, NULL, NULL, "nx", NULL, "mmxext", NULL, NULL, "fxsr_opt", "pdpe1gb", "rdtscp", NULL, "lm",
    "3dnowext", "3dnow",
};
static const char *const ext1_ecx_names[32] = {
    "lahf_lm", "cmp_legacy", "svm", "extapic", "cr8_legacy", "abm", "sse4a", "misalignsse", "3dnowprefetch",
    "osvw", "ibs", "xop", "skinit", "wdt", NULL, "lwp", "fma4", "tce", NULL, NULL, NULL, "tbm", "topoext",
    "perfctr_core", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
};

/* Append the names of the set bits of word to buf. */
static void feature_names(char *buf, size_t size, uint32_t word, const char *const names[32])
{
    for (unsigned bit = 0; bit < 32; bit++)
        if ((word & (1u << bit)) && names[bit])
            devinfo_append(buf, size, " ", names[bit]);
}

void cpu_describe(struct devinfo *d)
{
    const struct cpu_features *f = &cpu_features;
    devinfo_node(d, "cpu", "%s", f->brand[0] ? f->brand : f->vendor);
    devinfo_prop(d, "vendor", "%s", f->vendor);
    if (f->brand[0])
        devinfo_prop(d, "model_name", "%s", f->brand);
    devinfo_prop(d, "family", "%u (0x%x)", f->family, f->family);
    devinfo_prop(d, "model", "%u (0x%x)", f->model, f->model);
    devinfo_prop(d, "stepping", "%u", f->stepping);
    devinfo_prop(d, "signature", "0x%08x", f->signature);
    devinfo_prop(d, "hypervisor", "%s", f->hypervisor[0] ? f->hypervisor : "none");
    devinfo_prop(d, "cpus", "%u", smp_cpu_count());
    devinfo_prop(d, "physical_address_bits", "%u", f->phys_bits);
    devinfo_prop(d, "virtual_address_bits", "%u", f->virt_bits);
    devinfo_prop(d, "max_leaf", "0x%x", f->max_leaf);
    devinfo_prop(d, "max_extended_leaf", "0x%x", f->max_ext_leaf);
    devinfo_prop(d, "clock_rate", "%lu Hz (TSC)", (unsigned long)timer_clock_hz());
    devinfo_prop(d, "invariant_tsc", "%s", f->invariant_tsc ? "yes" : "no");
    char names[640];
    names[0] = '\0';
    feature_names(names, sizeof names, f->leaf1_edx, leaf1_edx_names);
    feature_names(names, sizeof names, f->leaf1_ecx, leaf1_ecx_names);
    devinfo_prop(d, "features", "%s", names);
    names[0] = '\0';
    feature_names(names, sizeof names, f->leaf7_ebx, leaf7_ebx_names);
    feature_names(names, sizeof names, f->leaf7_ecx, leaf7_ecx_names);
    feature_names(names, sizeof names, f->leaf7_edx, leaf7_edx_names);
    devinfo_prop(d, "extended_features", "%s", names);
    names[0] = '\0';
    feature_names(names, sizeof names, f->ext1_edx, ext1_edx_names);
    feature_names(names, sizeof names, f->ext1_ecx, ext1_ecx_names);
    devinfo_prop(d, "amd_features", "%s", names);
    devinfo_prop(d, "feature_words", "1:%08x:%08x 7:%08x:%08x:%08x 80000001:%08x:%08x", f->leaf1_ecx, f->leaf1_edx,
                 f->leaf7_ebx, f->leaf7_ecx, f->leaf7_edx, f->ext1_ecx, f->ext1_edx);
    for (unsigned i = 0; i < f->ncaches; i++) {
        const struct cpu_cache *k = &f->caches[i];
        const char *type = k->type == 'd' ? "data" : k->type == 'i' ? "instruction" : "unified";
        char path[24];
        ksnprintf(path, sizeof path, "cpu/cache%u", i);
        devinfo_node(d, path, "L%u %s cache, %u KiB", k->level, type, k->size / 1024);
        devinfo_prop(d, "level", "%u", k->level);
        devinfo_prop(d, "cache_type", "%s", type);
        devinfo_size(d, "cache_size", k->size);
        devinfo_prop(d, "ways", "%u", k->ways);
        devinfo_prop(d, "line_size", "%u bytes", k->line);
        if (k->sets)
            devinfo_prop(d, "sets", "%u", k->sets);
        if (k->shared)
            devinfo_prop(d, "shared_by", "%u logical processors", k->shared);
    }
    for (unsigned i = 0; i < smp_cpu_count(); i++) {
        struct cpu *c = cpu_by_id(i);
        char path[16];
        ksnprintf(path, sizeof path, "cpu/%u", i);
        devinfo_node(d, path, "CPU %u", i);
        devinfo_prop(d, "cpu_id", "%u", i);
        devinfo_prop(d, "apic_id", "%u", c->arch.lapic_id);
        devinfo_prop(d, "boot_cpu", "%s", i == 0 ? "yes" : "no");
        devinfo_prop(d, "started", "%s", __atomic_load_n(&c->started, __ATOMIC_ACQUIRE) ? "yes" : "no");
    }
}
