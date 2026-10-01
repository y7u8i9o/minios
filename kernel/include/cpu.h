#pragma once
#include <kernel.h>
#include <sync/mpsc.h>
#include <sync/spinlock.h>
#include <arch/percpu.h>

/* Upper bound on processors. cpu_mask values are one bit per CPU id. */
#define MAX_CPUS 16
typedef uint64_t cpu_mask_t;

struct thread;
struct vmspace;
struct page;

/* Per CPU state. Reached exclusively through cpu_current() (<arch/cpu.h>),
 * or through cpu_by_id(id) for another processor. Fields are private to
 * the owning CPU except where a comment says otherwise. The fields up to
 * arch keep fixed offsets for the entry code of the architecture. */
struct cpu {
    struct cpu *self;           /* must stay at offset 0 */
    uint32_t id;
    struct thread *current;     /* running thread, local run-queue lock */
    void *kstack_top;           /* top of the running thread's kernel stack */
    int cli_depth;              /* push_cli nesting depth */
    int int_enabled;            /* interrupts enabled before the outermost push_cli */
    struct vmspace *vm;         /* address space loaded in the MMU */
    struct arch_cpu arch;       /* architecture state (<arch/percpu.h>) */
    struct thread *idle;        /* this CPU's idle thread, local run-queue lock */
    struct thread *zombie_pending; /* switched-away zombie, local run-queue lock */
    bool need_resched;          /* local tick or reschedule IPI */
    volatile bool online;       /* runs kernel code on its own stack, set once */
    volatile bool started;      /* finished per CPU initialization, set once */
    void *ap_stack_top;         /* stack used from startup on, becomes the idle stack */
    uint64_t ticks;             /* local timer interrupts, written by this CPU only */
    uint64_t rcu_epoch;         /* last RCU quiescent epoch, release published */
    unsigned rcu_read_depth;    /* owning CPU only; read sections may not sleep */
    struct mpsc_head rcu_callbacks; /* producers local, reclaimed by this CPU */
    struct spinlock pmm_cache_lock; /* this CPU's single-page cache */
    struct page *pmm_cache[32];
    unsigned pmm_cache_count;
};

/* Return the structure of CPU id (0 is the boot CPU). Valid ids are below
 * smp_cpu_count(). */
struct cpu *cpu_by_id(unsigned id);

/* Interrupt disable nesting. push_cli disables interrupts and records the
 * previous state on first entry, pop_cli restores it when the depth hits
 * 0. The names are kept on every architecture: the profiler recognises
 * lock primitives by them (docs/design/arch.md). */
void push_cli(void);
void pop_cli(void);
