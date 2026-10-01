/* Program entry. The kernel leaves argc at [sp], argv above it and envp
 * after the NULL that ends argv, with sp 16 byte aligned. The loader
 * supplies its finalizer in x0 (the AArch64 ELF convention) and its
 * initializer in x1; the kernel enters a static program with both zero,
 * and libc ignores them for static programs (AT_BASE == 0). */
    .section .text
    .global _start
    .type _start, %function
_start:
    mov x29, #0
    mov x30, #0
    mov x4, x0                  /* loader finalizer, argument five */
    mov x3, x1                  /* loader initializer, argument four */
    ldr x0, [sp]                /* argc */
    add x1, sp, #8              /* argv */
    add x2, x1, x0, lsl #3
    add x2, x2, #8              /* envp = argv + argc + 1 */
    bl __libc_start
1:  brk #0
    b 1b
    .size _start, . - _start
    .section .note.GNU-stack,"",%progbits
