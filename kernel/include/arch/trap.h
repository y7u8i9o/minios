#pragma once
#include <kernel.h>

/* Register state saved by the interrupt stubs in isr.S. Layout must match
 * the push order there. */
struct trapframe {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector;
    uint64_t error;
    /* pushed by the CPU */
    uint64_t rip, cs, rflags, rsp, ss;
};

#define T_DIVIDE     0
#define T_DEBUG      1
#define T_NMI        2
#define T_BREAKPOINT 3
#define T_OVERFLOW   4
#define T_BOUND      5
#define T_ILLOP      6
#define T_DEVICE     7
#define T_DBLFLT     8
#define T_TSS       10
#define T_SEGNP     11
#define T_STACK     12
#define T_GPFLT     13
#define T_PGFLT     14
#define T_FPERR     16
#define T_ALIGN     17
#define T_MCHK      18
#define T_SIMDERR   19

#define T_IRQ0      32

void idt_init(void);
/* Load the shared IDT on the calling CPU. */
void idt_load(void);
void trap_dispatch(struct trapframe *tf);
void trap_dump_frame(const struct trapframe *tf);
