#pragma once
#include <kernel.h>
#include <cpu.h>
#include <arch/barrier.h>

#define MSR_EFER            0xc0000080
#define MSR_STAR            0xc0000081
#define MSR_LSTAR           0xc0000082
#define MSR_SFMASK          0xc0000084
#define MSR_FS_BASE         0xc0000100
#define MSR_GS_BASE         0xc0000101
#define MSR_KERNEL_GS_BASE  0xc0000102

#define RFLAGS_IF           (1UL << 9)

/* Offsets used by syscall.S. */
_Static_assert(offsetof(struct cpu, kstack_top) == 24, "cpu.kstack_top offset");
_Static_assert(offsetof(struct cpu, arch.user_rsp) == 48, "cpu.arch.user_rsp offset");

static inline struct cpu *cpu_current(void)
{
    struct cpu *c;
    __asm__ volatile("movq %%gs:0, %0" : "=r"(c));
    return c;
}

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t v)
{
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

static inline uint64_t read_rflags(void)
{
    uint64_t f;
    __asm__ volatile("pushfq; popq %0" : "=r"(f));
    return f;
}

static inline void cli(void)
{
    __asm__ volatile("cli" : : : "memory");
}

static inline void sti(void)
{
    __asm__ volatile("sti" : : : "memory");
}

/* Interrupt state for generic code. Interrupt disabling is never mutual
 * exclusion on its own (docs/design/locking.md). arch_irq_save disables
 * interrupts and returns the previous state for arch_irq_restore. */
static inline void arch_irq_enable(void)
{
    sti();
}

static inline void arch_irq_disable(void)
{
    cli();
}

static inline bool arch_irqs_enabled(void)
{
    return (read_rflags() & RFLAGS_IF) != 0;
}

/* True if the state saved by arch_irq_save had interrupts enabled. */
static inline bool arch_irq_flags_enabled(unsigned long flags)
{
    return (flags & RFLAGS_IF) != 0;
}

static inline unsigned long arch_irq_save(void)
{
    unsigned long flags = read_rflags();
    cli();
    return flags;
}

static inline void arch_irq_restore(unsigned long flags)
{
    if (flags & RFLAGS_IF)
        sti();
}

static inline void hlt(void)
{
    __asm__ volatile("hlt");
}

/* Wait for the next interrupt with the current interrupt state. */
static inline void arch_wait_for_interrupt(void)
{
    hlt();
}

/* Enable interrupts and wait for the next one (the idle loop). sti takes
 * effect after the following instruction, so no interrupt is taken between
 * the two and a wakeup cannot be lost before hlt. */
static inline void arch_idle(void)
{
    __asm__ volatile("sti; hlt" : : : "memory");
}

static inline __noreturn void cpu_halt_forever(void)
{
    for (;;) {
        cli();
        hlt();
    }
}

static inline uint64_t read_cr2(void)
{
    uint64_t v;
    __asm__ volatile("movq %%cr2, %0" : "=r"(v));
    return v;
}

static inline uint64_t read_cr3(void)
{
    uint64_t v;
    __asm__ volatile("movq %%cr3, %0" : "=r"(v));
    return v;
}

static inline uintptr_t read_rbp(void)
{
    uintptr_t v;
    __asm__ volatile("movq %%rbp, %0" : "=r"(v));
    return v;
}

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* Free running cycle counter for lock statistics and the profiler; the
 * unit is not calibrated. */
static inline uint64_t arch_cycles(void)
{
    return rdtsc();
}

/* Set up the boot CPU structure and load its GS base. */
void cpu_init_boot(void);
/* Processor identification and the features the kernel depends on, read
 * from CPUID once by the boot CPU (cpu_identify) and constant afterwards.
 * Every vendor or hypervisor dependent decision reads this structure;
 * no other file executes cpuid. See docs/design/platform.md. */
struct cpu_features {
    char vendor[13];            /* "GenuineIntel", "AuthenticAMD", ... */
    char hypervisor[13];        /* "KVMKVMKVM\0\0\0", "TCGTCGTCGTCG", ..., "" on bare hardware */
    unsigned family, model, stepping;
    bool intel, amd;
    bool nx;                    /* execute disable, EFER.NXE */
    bool pge;                   /* global pages, CR4.PGE */
    bool pat;                   /* page attribute table MSR */
    bool fxsr;                  /* fxsave/fxrstor, CR4.OSFXSR */
    bool sse2;
    bool x2apic;
    bool pcid;
    bool pdpe1gb;               /* 1 GiB pages */
    bool invariant_tsc;
    bool tsc_deadline;
    unsigned phys_bits;         /* physical address width */
    unsigned virt_bits;         /* linear address width */
    /* For /dev/devices (docs/design/sysinfo.md): the brand string, the
     * leaf ranges, the signature, the feature words and the caches. */
    char brand[49];
    uint32_t max_leaf, max_ext_leaf;
    uint32_t signature;         /* eax of leaf 1 */
    uint32_t leaf1_ecx, leaf1_edx;
    uint32_t leaf7_ebx, leaf7_ecx, leaf7_edx;
    uint32_t ext1_ecx, ext1_edx;
    unsigned ncaches;
    struct cpu_cache {
        uint8_t level;
        char type;              /* 'd' data, 'i' instruction, 'u' unified */
        uint16_t ways;
        uint16_t line;          /* bytes */
        uint16_t shared;        /* logical processors sharing it, 0 if unknown */
        uint32_t sets;          /* 0 if unknown */
        uint32_t size;          /* bytes */
    } caches[8];
};
extern struct cpu_features cpu_features;

/* Fill cpu_features from CPUID and log the processor. Boot CPU only,
 * before any feature is enabled. */
void cpu_identify(void);
struct devinfo;
/* The processor part of /dev/devices, from cpu_features (cpu.c). */
void cpu_describe(struct devinfo *d);
/* The local APIC and the I/O APIC part of /dev/devices (apic.c). */
void apic_describe(struct devinfo *d);
