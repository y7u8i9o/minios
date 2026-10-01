#include <syscall_nums.h>

/* long __syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5) */
    .section .text
    .global __syscall6
__syscall6:
    movq %rdi, %rax
    movq %rsi, %rdi
    movq %rdx, %rsi
    movq %rcx, %rdx
    movq %r8, %r10
    movq %r9, %r8
    movq 8(%rsp), %r9
    syscall
    ret

/* Signal handlers return here: rsp points at the saved trap frame. */
    .globl __sigreturn_trampoline
    .type __sigreturn_trampoline, @function
__sigreturn_trampoline:
    movl $SYS_sigreturn, %eax
    syscall
    hlt
