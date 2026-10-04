#pragma once
#include <stddef.h>

/* minios user threads: kernel scheduled threads sharing the address space.
 * The caller supplies the stack. */
typedef int thread_t;
int thread_create(thread_t *out, void (*fn)(void *), void *arg, void *stack, size_t stack_size);
__attribute__((noreturn)) void thread_exit(int code);
int thread_join(thread_t t, int *code);
