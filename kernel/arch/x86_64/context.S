/* void context_switch(uint64_t **old_sp, uint64_t *new_sp)
 *
 * Saves the callee saved registers on the current stack, stores the stack
 * pointer through old_sp, loads new_sp and restores the registers saved
 * there. A new thread's stack is prepared by thread_alloc to look like a
 * frame saved here whose return address is thread_start. */
    .section .text
    .global context_switch
    .type context_switch, @function
context_switch:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, (%rdi)
    movq %rsi, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rbx
    popq %rbp
    ret
    .size context_switch, . - context_switch
