#include <syscall_nums.h>

/* long __syscall6(long nr, long a0, long a1, long a2, long a3, long a4, long a5)
 * The kernel takes the number in x8 and the arguments in x0 to x5, and
 * returns the result in x0. */
    .section .text
    .global __syscall6
    .type __syscall6, %function
__syscall6:
    mov x8, x0
    mov x0, x1
    mov x1, x2
    mov x2, x3
    mov x3, x4
    mov x4, x5
    mov x5, x6
    svc #0
    ret
    .size __syscall6, . - __syscall6

/* Signal handlers return here (x30 of the handler): sp points at the
 * saved frame. */
    .globl __sigreturn_trampoline
    .type __sigreturn_trampoline, %function
__sigreturn_trampoline:
    mov x8, #SYS_sigreturn
    svc #0
    brk #0
    .size __sigreturn_trampoline, . - __sigreturn_trampoline
    .section .note.GNU-stack,"",%progbits
