#pragma once

/* The x86_64 primitives that the C code of libc uses (internal header,
 * found through -Iarch/$(ARCH)). */

/* The thread pointer: the address of the running thread's control block,
 * whose first word points to itself (TLS variant II, the FS base). */
static inline void *__arch_thread_pointer(void)
{
    void *self;
    __asm__ volatile("movq %%fs:0, %0" : "=r"(self));
    return self;
}

/* The initial stack pointer of a new thread whose stack ends at the 16
 * byte aligned address top. The kernel enters the C function directly,
 * without a call instruction, so a return address slot is supplied: the
 * stack pointer is then 8 modulo 16 at function entry, as the x86-64 ABI
 * requires (including aligned SIMD stack accesses). */
static inline unsigned long __arch_thread_stack_top(unsigned long top)
{
    top -= sizeof(unsigned long);
    *(unsigned long *)top = 0;
    return top;
}

/* Hint inside a spin wait loop. */
static inline void __arch_spin_hint(void)
{
    __asm__ volatile("pause");
}
