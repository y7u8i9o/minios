#pragma once

/* The aarch64 primitives that the C code of libc uses (internal header,
 * found through -Iarch/$(ARCH)). */

/* The thread pointer (TPIDR_EL0): the thread control block of TLS variant
 * I, whose first word points to the running thread's struct pthread
 * (minios/dl.h). */
static inline void *__arch_thread_pointer(void)
{
    void *tp;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp));
    return tp;
}

/* The initial stack pointer of a new thread whose stack ends at the 16
 * byte aligned address top: the AArch64 procedure call standard needs no
 * return address slot. */
static inline unsigned long __arch_thread_stack_top(unsigned long top)
{
    return top;
}

/* Hint inside a spin wait loop. */
static inline void __arch_spin_hint(void)
{
    __asm__ volatile("yield");
}
