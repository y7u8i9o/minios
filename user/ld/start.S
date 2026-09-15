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

/* Lazy binding. The first entry of a procedure linkage table pushes the
 * object record the loader stored in its GOT and jumps here; the entry
 * that was called pushed the index of its relocation before that. Every
 * register an argument may travel in is preserved around _dl_fixup,
 * including the vector registers of floating point arguments, and the
 * two pushed words are dropped before the call continues to the target. */
    .global _dl_runtime_resolve
    .hidden _dl_runtime_resolve
    .type _dl_runtime_resolve, @function
_dl_runtime_resolve:
    pushq %rbp
    movq %rsp, %rbp
    subq $192, %rsp
    movq %rax, 0(%rsp)
    movq %rcx, 8(%rsp)
    movq %rdx, 16(%rsp)
    movq %rsi, 24(%rsp)
    movq %rdi, 32(%rsp)
    movq %r8, 40(%rsp)
    movq %r9, 48(%rsp)
    movq %r10, 56(%rsp)
    movdqa %xmm0, 64(%rsp)
    movdqa %xmm1, 80(%rsp)
    movdqa %xmm2, 96(%rsp)
    movdqa %xmm3, 112(%rsp)
    movdqa %xmm4, 128(%rsp)
    movdqa %xmm5, 144(%rsp)
    movdqa %xmm6, 160(%rsp)
    movdqa %xmm7, 176(%rsp)
    movq 8(%rbp), %rdi          /* the object */
    movq 16(%rbp), %rsi         /* the relocation index */
    call _dl_fixup
    movq %rax, %r11
    movdqa 176(%rsp), %xmm7
    movdqa 160(%rsp), %xmm6
    movdqa 144(%rsp), %xmm5
    movdqa 128(%rsp), %xmm4
    movdqa 112(%rsp), %xmm3
    movdqa 96(%rsp), %xmm2
    movdqa 80(%rsp), %xmm1
    movdqa 64(%rsp), %xmm0
    movq 56(%rsp), %r10
    movq 48(%rsp), %r9
    movq 40(%rsp), %r8
    movq 32(%rsp), %rdi
    movq 24(%rsp), %rsi
    movq 16(%rsp), %rdx
    movq 8(%rsp), %rcx
    movq 0(%rsp), %rax
    movq %rbp, %rsp
    popq %rbp
    addq $16, %rsp              /* the object and the index */
    jmp *%r11

/* The recovery point of the runtime entries: die returns here with the
 * diagnostic stored, instead of exiting, while a dlopen or dlsym runs. */
    .global _dl_setjmp
    .hidden _dl_setjmp
    .type _dl_setjmp, @function
_dl_setjmp:
    movq %rbx, 0(%rdi)
    movq %rbp, 8(%rdi)
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

    .global _dl_longjmp
    .hidden _dl_longjmp
    .type _dl_longjmp, @function
_dl_longjmp:
    movq 0(%rdi), %rbx
    movq 8(%rdi), %rbp
    movq 16(%rdi), %r12
    movq 24(%rdi), %r13
    movq 32(%rdi), %r14
    movq 40(%rdi), %r15
    movq 48(%rdi), %rsp
    movl %esi, %eax
    jmp *56(%rdi)
    .section .note.GNU-stack,"",@progbits
