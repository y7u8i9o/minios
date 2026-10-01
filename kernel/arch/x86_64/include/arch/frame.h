#pragma once
#include <arch/trap.h>
#include <arch/gdt.h>

/* Accessors for the register state saved at a trap, an interrupt or a
 * system call. Generic code reads and writes a struct trapframe only
 * through these, so that it never names an x86 register
 * (docs/design/arch.md). */

static inline uintptr_t frame_pc(const struct trapframe *tf)
{
    return tf->rip;
}

static inline uintptr_t frame_sp(const struct trapframe *tf)
{
    return tf->rsp;
}

/* Frame pointer, the head of the rbp chain the unwinders follow. */
static inline uintptr_t frame_fp(const struct trapframe *tf)
{
    return tf->rbp;
}

/* True if the frame was saved on an entry from user mode. */
static inline bool frame_from_user(const struct trapframe *tf)
{
    return (tf->cs & 3) == 3;
}

static inline void frame_set_pc(struct trapframe *tf, uintptr_t pc)
{
    tf->rip = pc;
}

static inline void frame_set_sp(struct trapframe *tf, uintptr_t sp)
{
    tf->rsp = sp;
}

/* First argument register of a function entered with this frame. */
static inline void frame_set_arg0(struct trapframe *tf, uint64_t v)
{
    tf->rdi = v;
}

/* Return value register: the result of a system call, 0 in the child of
 * fork. */
static inline uint64_t frame_retval(const struct trapframe *tf)
{
    return tf->rax;
}

static inline void frame_set_retval(struct trapframe *tf, uint64_t v)
{
    tf->rax = v;
}

/* System call number and arguments, in the order of the SysV calling
 * convention with r10 in place of rcx, which syscall overwrites. */
static inline uint64_t frame_syscall_nr(const struct trapframe *tf)
{
    return tf->rax;
}

#define SYSARG0(tf) ((tf)->rdi)
#define SYSARG1(tf) ((tf)->rsi)
#define SYSARG2(tf) ((tf)->rdx)
#define SYSARG3(tf) ((tf)->r10)
#define SYSARG4(tf) ((tf)->r8)
#define SYSARG5(tf) ((tf)->r9)

/* The cause of a page fault, decoded from the frame of the fault. */
struct fault_info {
    bool present;               /* the page was mapped: a protection fault */
    bool write;
    bool user;                  /* the access came from user mode */
    bool exec;                  /* an instruction fetch */
};

static inline void arch_fault_decode(const struct trapframe *tf, struct fault_info *fi)
{
    /* The #PF error code: P, W/R, U/S, RSVD, I/D. */
    fi->present = (tf->error & 1) != 0;
    fi->write = (tf->error & 2) != 0;
    fi->user = (tf->error & 4) != 0;
    fi->exec = (tf->error & 16) != 0;
}

/* Prepare tf to enter user mode at pc with stack pointer sp and every
 * other register zero, with interrupts enabled. */
static inline void arch_frame_init_user(struct trapframe *tf, uintptr_t pc, uintptr_t sp)
{
    *tf = (struct trapframe){
        .rip = pc,
        .cs = GDT_USER_CODE | 3,
        .rflags = (1UL << 9) | 0x2,     /* IF and the reserved bit 1 */
        .rsp = sp,
        .ss = GDT_USER_DATA | 3,
    };
}
