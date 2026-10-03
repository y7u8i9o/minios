/* Entry of the dynamic loader. The kernel enters here with the initial
 * stack of the program (argc at [sp]) and no other state. _dl_main
 * receives the stack pointer, loads the libraries and returns the entry
 * point of the program, which is entered with the same stack pointer, the
 * finalizer in x0 (the AArch64 ELF convention) and, a MiniOS addition, the
 * initializer in x1, called by libc after its own setup. */
    .section .text
    .global _dl_start
    .type _dl_start, %function
_dl_start:
    mov x0, sp
    mov x29, #0
    mov x30, #0
    bl _dl_main
    mov x16, x0
    adrp x0, _dl_finalize
    add x0, x0, :lo12:_dl_finalize
    adrp x1, _dl_initialize
    add x1, x1, :lo12:_dl_initialize
    mov x29, #0
    mov x30, #0
    br x16
    .size _dl_start, . - _dl_start

/* Lazy binding. The first entry of a procedure linkage table pushes x16
 * (the address of the GOT slot the called entry loaded) and x30, sets x16
 * to the address of GOT[2] and jumps here through GOT[2]. GOT[1] contains
 * the object record. The slot GOT[3 + i] belongs to the relocation i of
 * the jump slot table. Every register an argument may travel in is
 * preserved around _dl_fixup: x0 to x8 and q0 to q7. */
    .global _dl_runtime_resolve
    .hidden _dl_runtime_resolve
    .type _dl_runtime_resolve, %function
_dl_runtime_resolve:
    stp x29, x30, [sp, #-224]!
    mov x29, sp
    stp x0, x1, [sp, #16]
    stp x2, x3, [sp, #32]
    stp x4, x5, [sp, #48]
    stp x6, x7, [sp, #64]
    str x8, [sp, #80]
    stp q0, q1, [sp, #96]
    stp q2, q3, [sp, #128]
    stp q4, q5, [sp, #160]
    stp q6, q7, [sp, #192]
    ldr x0, [x16, #-8]          /* GOT[1]: the object */
    ldr x1, [sp, #224]          /* the slot pushed by the first entry */
    sub x1, x1, x16
    sub x1, x1, #8
    lsr x1, x1, #3              /* (slot - &GOT[3]) / 8: the relocation index */
    bl _dl_fixup
    mov x17, x0
    ldp q6, q7, [sp, #192]
    ldp q4, q5, [sp, #160]
    ldp q2, q3, [sp, #128]
    ldp q0, q1, [sp, #96]
    ldr x8, [sp, #80]
    ldp x6, x7, [sp, #64]
    ldp x4, x5, [sp, #48]
    ldp x2, x3, [sp, #32]
    ldp x0, x1, [sp, #16]
    ldp x29, x30, [sp], #224
    ldp x16, x30, [sp], #16     /* the two words the first entry pushed */
    br x17
    .size _dl_runtime_resolve, . - _dl_runtime_resolve

/* The recovery point of the runtime entries: die returns here with the
 * diagnostic stored, instead of exiting, while a dlopen or dlsym runs. */
    .global _dl_setjmp
    .hidden _dl_setjmp
    .type _dl_setjmp, %function
_dl_setjmp:
    stp x19, x20, [x0, #0]
    stp x21, x22, [x0, #16]
    stp x23, x24, [x0, #32]
    stp x25, x26, [x0, #48]
    stp x27, x28, [x0, #64]
    stp x29, x30, [x0, #80]
    mov x2, sp
    str x2, [x0, #96]
    mov w0, #0
    ret
    .size _dl_setjmp, . - _dl_setjmp

    .global _dl_longjmp
    .hidden _dl_longjmp
    .type _dl_longjmp, %function
_dl_longjmp:
    ldp x19, x20, [x0, #0]
    ldp x21, x22, [x0, #16]
    ldp x23, x24, [x0, #32]
    ldp x25, x26, [x0, #48]
    ldp x27, x28, [x0, #64]
    ldp x29, x30, [x0, #80]
    ldr x2, [x0, #96]
    mov sp, x2
    mov w0, w1
    ret
    .size _dl_longjmp, . - _dl_longjmp
    .section .note.GNU-stack,"",%progbits
