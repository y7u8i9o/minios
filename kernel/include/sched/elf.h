#pragma once
#include <kernel.h>

struct vmspace;

/* Load a static ELF64 executable into vm: one region per PT_LOAD segment
 * plus a heap region right after the highest segment. Returns the entry
 * point through *entry. */
int elf_load(struct vmspace *vm, const void *image, size_t size, uintptr_t *entry);

/* Create the stack region and lay out argc, argv and envp on it following
 * the SysV ABI. Returns the initial stack pointer through *rsp. */
/* Build the main stack of stack_size bytes (already clamped) below USER_STACK_TOP. */
int user_stack_setup(struct vmspace *vm, char *const argv[], char *const envp[], uintptr_t *rsp,
                     size_t stack_size);
