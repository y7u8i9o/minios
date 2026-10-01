#pragma once
#include <kernel.h>

/* Print the frame pointer chain starting at the caller. */
void backtrace_print(void);
/* Print a chain starting from a saved rip and rbp pair. */
void backtrace_print_from(uintptr_t rip, uintptr_t rbp);

struct thread;
/* The kernel frames of t from pc and the frame pointer fp, within its
 * kernel stack (debug/unwind.c). */
unsigned unwind_kernel(const struct thread *t, uintptr_t pc, uintptr_t fp, uint64_t *chain, unsigned max);
/* The user program counter and frame pointer at the entry of t into the
 * kernel, false for a kernel thread. */
bool unwind_user_entry(const struct thread *t, uintptr_t *pc, uintptr_t *fp);
