/* void context_switch(uint64_t **old_sp, uint64_t *new_sp)
 *
 * Saves the callee saved registers x19 to x30 on the current stack, stores
 * the stack pointer through old_sp, loads new_sp and restores the
 * registers saved there. A new thread's stack is prepared by
 * arch_thread_init to look like a frame saved here whose x30 is the
 * thread's first function. */
    .section .text
    .global context_switch
    .type context_switch, %function
context_switch:
    sub sp, sp, #96
    stp x19, x20, [sp, #0]
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    mov x9, sp
    str x9, [x0]
    mov sp, x1
    ldp x19, x20, [sp, #0]
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    add sp, sp, #96
    ret
    .size context_switch, . - context_switch
