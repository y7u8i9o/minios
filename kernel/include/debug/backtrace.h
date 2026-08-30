#pragma once
#include <kernel.h>

/* Print the frame pointer chain starting at the caller. */
void backtrace_print(void);
/* Print a chain starting from a saved rip and rbp pair. */
void backtrace_print_from(uintptr_t rip, uintptr_t rbp);
