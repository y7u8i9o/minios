/* Program entry. The kernel leaves argc at (%rsp), argv above it and envp
 * after the NULL that ends argv, with rsp 16 byte aligned. The loader
 * supplies its initializer in rcx and finalizer in rdx. libc ignores these
 * registers for static programs (AT_BASE == 0), since sysret uses rcx for
 * the entry address. Preserve the finalizer as argument five before using
 * rdx for the environment. */
    .section .text
    .global _start
_start:
    xorl %ebp, %ebp
    movq %rdx, %r8             /* loader finalizer; rcx remains argument four */
    movq (%rsp), %rdi           /* argc */
    leaq 8(%rsp), %rsi          /* argv */
    leaq 8(%rsi,%rdi,8), %rdx   /* envp = argv + argc + 1 */
    andq $-16, %rsp
    call __libc_start
1:  hlt
    jmp 1b
    .section .note.GNU-stack,"",@progbits
