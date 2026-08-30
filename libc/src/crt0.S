/* Program entry. The kernel leaves argc at (%rsp), argv above it and envp
 * after the NULL that ends argv, with rsp 16 byte aligned. */
    .section .text
    .global _start
_start:
    xorl %ebp, %ebp
    movq (%rsp), %rdi           /* argc */
    leaq 8(%rsp), %rsi          /* argv */
    leaq 8(%rsi,%rdi,8), %rdx   /* envp = argv + argc + 1 */
    andq $-16, %rsp
    call __libc_start
1:  hlt
    jmp 1b
