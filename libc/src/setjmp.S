/* setjmp/longjmp for the x86-64 System V ABI.  A jump restores the state
 * that the caller would observe immediately after setjmp returned. */
    .section .text

    .globl setjmp
    .type setjmp, @function
setjmp:
    movq %rbx,  0(%rdi)
    movq %rbp,  8(%rdi)
    movq %r12, 16(%rdi)
    movq %r13, 24(%rdi)
    movq %r14, 32(%rdi)
    movq %r15, 40(%rdi)
    leaq 8(%rsp), %rax
    movq %rax, 48(%rdi)
    movq (%rsp), %rax
    movq %rax, 56(%rdi)
    xorl %eax, %eax
    ret
    .size setjmp, .-setjmp

    .globl longjmp
    .type longjmp, @function
longjmp:
    movq  0(%rdi), %rbx
    movq  8(%rdi), %rbp
    movq 16(%rdi), %r12
    movq 24(%rdi), %r13
    movq 32(%rdi), %r14
    movq 40(%rdi), %r15
    movq 48(%rdi), %rsp
    movq 56(%rdi), %rdx
    movl %esi, %eax
    testl %eax, %eax
    jne 1f
    movl $1, %eax
1:
    jmp *%rdx
    .size longjmp, .-longjmp

    .section .note.GNU-stack,"",@progbits

/* POSIX names: minios has no signal mask to save, so both pairs agree. */
    .globl _setjmp
    .set _setjmp, setjmp
    .globl _longjmp
    .set _longjmp, longjmp
