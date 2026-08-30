/* Interrupt and exception entry stubs. Every vector pushes a dummy error code
 * when the CPU does not, then its vector number, and joins isr_common which
 * builds a struct trapframe and calls trap_dispatch. */

.macro ISR_NOERR vec
    .align 8
isr_\vec:
    pushq $0
    pushq $\vec
    jmp isr_common
.endm

.macro ISR_ERR vec
    .align 8
isr_\vec:
    pushq $\vec
    jmp isr_common
.endm

    .section .text

isr_common:
    /* rsp -> vector, error, rip, cs: swap to the kernel GS if from ring 3 */
    testb $3, 24(%rsp)
    jz 1f
    swapgs
1:
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    cld
    movq %rsp, %rdi
    call trap_dispatch
    .global isr_return
isr_return:
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $16, %rsp
    testb $3, 8(%rsp)
    jz 2f
    swapgs
2:
    iretq

/* void user_enter(struct trapframe *tf): load a frame and return to it. */
    .global user_enter
    .type user_enter, @function
user_enter:
    movq %rdi, %rsp
    jmp isr_return
    .size user_enter, . - user_enter

    ISR_NOERR 0
    ISR_NOERR 1
    ISR_NOERR 2
    ISR_NOERR 3
    ISR_NOERR 4
    ISR_NOERR 5
    ISR_NOERR 6
    ISR_NOERR 7
    ISR_ERR   8
    ISR_NOERR 9
    ISR_ERR   10
    ISR_ERR   11
    ISR_ERR   12
    ISR_ERR   13
    ISR_ERR   14
    ISR_NOERR 15
    ISR_NOERR 16
    ISR_ERR   17
    ISR_NOERR 18
    ISR_NOERR 19
    ISR_NOERR 20
    ISR_ERR   21
    ISR_NOERR 22
    ISR_NOERR 23
    ISR_NOERR 24
    ISR_NOERR 25
    ISR_NOERR 26
    ISR_NOERR 27
    ISR_NOERR 28
    ISR_ERR   29
    ISR_ERR   30
    ISR_NOERR 31

    .altmacro
    .set i, 32
    .rept 224
    ISR_NOERR %i
    .set i, i + 1
    .endr

.macro ISR_ENTRY vec
    .quad isr_\vec
.endm

    .section .rodata
    .global isr_table
    .align 8
isr_table:
    .set i, 0
    .rept 256
    ISR_ENTRY %i
    .set i, i + 1
    .endr
