/* Exception vectors (VBAR_EL1). Every entry saves a struct trapframe
 * (arch/trap.h) on the current stack and calls trap_dispatch. The table
 * has four groups of four entries: current EL with SP_EL0, current EL with
 * SP_ELx, lower EL in AArch64, lower EL in AArch32. */
#define FRAME_SIZE  304             /* sizeof(struct trapframe) */
#define F_SP        248
#define F_PC        256
#define F_PSTATE    264
#define F_ESR       272
#define F_FAR       280
#define F_KIND      288

.macro save_frame kind, lower
    sub sp, sp, #FRAME_SIZE
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]
    .if \lower
    mrs x0, sp_el0
    .else
    add x0, sp, #FRAME_SIZE
    .endif
    str x0, [sp, #F_SP]
    mrs x0, elr_el1
    str x0, [sp, #F_PC]
    mrs x0, spsr_el1
    str x0, [sp, #F_PSTATE]
    mrs x0, esr_el1
    str x0, [sp, #F_ESR]
    mrs x0, far_el1
    str x0, [sp, #F_FAR]
    mov x0, #(\kind + 4 * \lower)
    str x0, [sp, #F_KIND]
.endm

.macro vector kind, lower
    .balign 128
    save_frame \kind, \lower
    mov x0, sp
    b trap_entry_common
.endm

    .section .text
    .balign 2048
    .global exception_vectors
exception_vectors:
    vector 0, 0         /* current EL, SP_EL0: not used by the kernel */
    vector 1, 0
    vector 2, 0
    vector 3, 0
    vector 0, 0         /* current EL, SP_ELx */
    vector 1, 0
    vector 2, 0
    vector 3, 0
    vector 0, 1         /* lower EL, AArch64 */
    vector 1, 1
    vector 2, 1
    vector 3, 1
    vector 0, 1         /* lower EL, AArch32: not supported */
    vector 1, 1
    vector 2, 1
    vector 3, 1

/* x0 = the frame. The frame record links the trap into backtraces. */
trap_entry_common:
    ldr x1, [x0, #F_PC]
    stp x29, x1, [sp, #-16]!
    mov x29, sp
    bl trap_dispatch
    add sp, sp, #16
    /* Restore the state the handler may have changed and return. */
    ldr x0, [sp, #F_PC]
    msr elr_el1, x0
    ldr x0, [sp, #F_PSTATE]
    msr spsr_el1, x0
    ldr x0, [sp, #F_KIND]
    tbz x0, #2, 1f
    ldr x0, [sp, #F_SP]
    msr sp_el0, x0
1:  ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
    ldp x0, x1, [sp, #0]
    add sp, sp, #FRAME_SIZE
    eret
