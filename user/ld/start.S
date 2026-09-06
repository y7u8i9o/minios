/* Entry of the dynamic loader. The kernel enters here with the initial
 * stack of the program (argc at (%rsp)) and no other state. _dl_main
 * receives the stack pointer, loads the libraries and returns the entry
 * point of the program, which is entered with the same stack pointer.
 * %rdx supplies the finalizer (the x86-64 ELF convention); MiniOS also
 * passes an initializer in %rcx, called by libc after its own setup. */
    .section .text
    .global _dl_start
    .type _dl_start, @function
_dl_start:
    movq %rsp, %rbx
    movq %rsp, %rdi
    andq $-16, %rsp
    call _dl_main
    movq %rbx, %rsp
    xorl %ebp, %ebp
    leaq _dl_finalize(%rip), %rdx
    leaq _dl_initialize(%rip), %rcx
    jmp *%rax
    .section .note.GNU-stack,"",@progbits
