/* System call entry. On `syscall` the CPU has loaded the kernel CS/SS,
 * saved rip in rcx and rflags in r11, and masked IF (SFMASK). The user
 * stack is still active, so the first step is to swap to the kernel GS and
 * switch to the thread's kernel stack, then build a struct trapframe with
 * the same layout the interrupt stubs produce. */
#define CPU_KSTACK_TOP  24
#define CPU_USER_RSP    48
#define USER_CS         0x23
#define USER_SS         0x1b
#define SYSCALL_VECTOR  256

    .section .text
    .global syscall_entry
    .type syscall_entry, @function
syscall_entry:
    swapgs
    movq %rsp, %gs:CPU_USER_RSP
    movq %gs:CPU_KSTACK_TOP, %rsp
    pushq $USER_SS
    pushq %gs:CPU_USER_RSP
    pushq %r11
    pushq $USER_CS
    pushq %rcx
    pushq $0
    pushq $SYSCALL_VECTOR
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
    call syscall_dispatch
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
    /* rsp -> vector, error, rip, cs, rflags, rsp, ss */
    movq 16(%rsp), %rcx
    movq 32(%rsp), %r11
    swapgs
    movq 40(%rsp), %rsp
    sysretq
    .size syscall_entry, . - syscall_entry
