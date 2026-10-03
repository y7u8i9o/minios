#pragma once
#include <kernel.h>

struct vmspace;

/* What the loader learned about an image. For a dynamically linked program
 * interp names the loader from PT_INTERP, and after elf_load_interp base and
 * interp_entry describe where that loader was placed. */
struct elf_info {
    uintptr_t entry;            /* entry point of the program */
    uintptr_t phdr;             /* address of the program headers in memory, 0 if unmapped */
    unsigned phnum;
    void *phdr_copy;            /* kmalloc'd copy of unmapped headers for the stack, else NULL */
    char interp[64];            /* PT_INTERP path, empty for a static program */
    uintptr_t interp_base;
    uintptr_t interp_entry;
    bool secure;                /* AT_SECURE: the program changes the ids (U2) */
};

/* Load an ELF64 executable (ET_EXEC) into vm: one region per PT_LOAD segment
 * plus a heap region right after the highest segment. */
int elf_load(struct vmspace *vm, const void *image, size_t size, struct elf_info *info);

/* Load a position independent loader (ET_DYN) at base and record its entry
 * point in info. */
int elf_load_interp(struct vmspace *vm, const void *image, size_t size, uintptr_t base,
                    struct elf_info *info);

/* Build the main stack of stack_size bytes (already clamped) below
 * USER_STACK_TOP: argc, argv, envp and the auxiliary vector following the
 * SysV ABI. Returns the initial stack pointer through *rsp. */
int user_stack_setup(struct vmspace *vm, char *const argv[], char *const envp[], uintptr_t *rsp,
                     size_t stack_size, const struct elf_info *info);

/* Auxiliary vector entries. */
#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_PAGESZ 6
#define AT_BASE   7
#define AT_ENTRY  9
#define AT_SECURE 23
