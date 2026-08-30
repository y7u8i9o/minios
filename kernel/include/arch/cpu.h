#pragma once
#include <kernel.h>

#define MSR_EFER            0xc0000080
#define MSR_STAR            0xc0000081
#define MSR_LSTAR           0xc0000082
#define MSR_SFMASK          0xc0000084
#define MSR_FS_BASE         0xc0000100
#define MSR_GS_BASE         0xc0000101
#define MSR_KERNEL_GS_BASE  0xc0000102

#define RFLAGS_IF           (1UL << 9)

/* Upper bound on processors. cpu_mask values are one bit per CPU id. */
#define MAX_CPUS 16
typedef uint64_t cpu_mask_t;

struct thread;
struct vmspace;

/* Per CPU state. Reached exclusively through cpu_current(), which reads the
 * self pointer at GS base offset 0, or through smp_cpu(id) for another
 * processor. Fields are private to the owning CPU except where a comment
 * says otherwise. The first fields keep fixed offsets for syscall.S. */
struct cpu {
    struct cpu *self;           /* must stay at offset 0 */
    uint32_t id;
    uint32_t lapic_id;
    struct thread *current;     /* running thread, sched_lock */
    void *kstack_top;           /* top of the running thread's kernel stack */
    int cli_depth;              /* push_cli nesting depth */
    int int_enabled;            /* IF before the outermost push_cli */
    struct vmspace *vm;         /* address space loaded in CR3 */
    uint64_t user_rsp;          /* scratch for the syscall entry */
    struct thread *idle;        /* this CPU's idle thread, sched_lock */
    struct thread *zombie_pending; /* switched away from, joiners not yet woken, sched_lock */
    bool need_resched;          /* set by the local timer tick, sched_lock */
    volatile bool online;       /* runs kernel code on its own stack, set once */
    volatile bool started;      /* finished per CPU initialization, set once */
    void *ap_stack_top;         /* stack used from startup on, becomes the idle stack */
    uint64_t ticks;             /* local timer interrupts, written by this CPU only */
};

/* Offsets used by syscall.S. */
_Static_assert(offsetof(struct cpu, kstack_top) == 24, "cpu.kstack_top offset");
_Static_assert(offsetof(struct cpu, user_rsp) == 48, "cpu.user_rsp offset");

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

static inline void hlt(void)
{
    __asm__ volatile("hlt");
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

static inline void cpu_relax(void)
{
    __asm__ volatile("pause");
}

/* Set up the boot CPU structure and load its GS base. */
void cpu_init_boot(void);
/* Return the structure of CPU id (0 is the boot CPU). Valid ids are below
 * smp_cpu_count(). */
struct cpu *cpu_by_id(unsigned id);

/* Interrupt disable nesting. push_cli disables interrupts and records the
 * previous state on first entry, pop_cli restores it when the depth hits 0. */
void push_cli(void);
void pop_cli(void);
