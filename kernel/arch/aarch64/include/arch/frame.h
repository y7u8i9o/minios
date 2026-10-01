#pragma once
#include <arch/trap.h>

/* Accessors for the saved register state (docs/design/arch.md). The
 * AArch64 procedure call standard passes arguments in x0 to x7 and returns
 * in x0; the system call number is in x8. */

static inline uintptr_t frame_pc(const struct trapframe *tf)
{
    return tf->pc;
}

static inline uintptr_t frame_sp(const struct trapframe *tf)
{
    return tf->sp;
}

/* Frame pointer x29, the head of the frame record chain. */
static inline uintptr_t frame_fp(const struct trapframe *tf)
{
    return tf->x[29];
}

/* True if the frame was saved on an entry from EL0 (SPSR.M = EL0t). */
static inline bool frame_from_user(const struct trapframe *tf)
{
    return (tf->pstate & 0xf) == 0;
}

static inline void frame_set_pc(struct trapframe *tf, uintptr_t pc)
{
    tf->pc = pc;
}

static inline void frame_set_sp(struct trapframe *tf, uintptr_t sp)
{
    tf->sp = sp;
}

static inline void frame_set_arg0(struct trapframe *tf, uint64_t v)
{
    tf->x[0] = v;
}

static inline uint64_t frame_retval(const struct trapframe *tf)
{
    return tf->x[0];
}

static inline void frame_set_retval(struct trapframe *tf, uint64_t v)
{
    tf->x[0] = v;
}

static inline uint64_t frame_syscall_nr(const struct trapframe *tf)
{
    return tf->x[8];
}

#define SYSARG0(tf) ((tf)->x[0])
#define SYSARG1(tf) ((tf)->x[1])
#define SYSARG2(tf) ((tf)->x[2])
#define SYSARG3(tf) ((tf)->x[3])
#define SYSARG4(tf) ((tf)->x[4])
#define SYSARG5(tf) ((tf)->x[5])

/* The cause of a page fault, decoded from the frame of the fault. */
struct fault_info {
    bool present;               /* a permission or access flag fault: the entry is valid */
    bool write;
    bool user;                  /* the access came from EL0 */
    bool exec;                  /* an instruction fetch */
};

static inline void arch_fault_decode(const struct trapframe *tf, struct fault_info *fi)
{
    unsigned ec = ESR_EC(tf->esr);
    unsigned fsc = tf->esr & 0x3f;      /* fault status code */
    fi->exec = ec == EC_IABT_LOWER || ec == EC_IABT_CUR;
    fi->user = ec == EC_IABT_LOWER || ec == EC_DABT_LOWER;
    fi->write = !fi->exec && (tf->esr & (1UL << 6)) != 0;   /* WnR */
    /* Translation faults (0b0001xx) find no valid entry; access flag
     * (0b0010xx) and permission faults (0b0011xx) find one. */
    fi->present = (fsc & 0x3c) == 0x08 || (fsc & 0x3c) == 0x0c;
}

/* Prepare tf to enter EL0 at pc with stack pointer sp and every other
 * register zero, with interrupts unmasked. */
static inline void arch_frame_init_user(struct trapframe *tf, uintptr_t pc, uintptr_t sp)
{
    *tf = (struct trapframe){ .pc = pc, .sp = sp, .pstate = 0 };
}
