#pragma once
#include <stdint.h>

/* The x86_64 part of the dynamic loader (docs/design/dynlink.md): the
 * relocation types it applies under generic names, the system call
 * instruction, the thread pointer and the initial-exec TLS offset. The
 * entry and the lazy binding trampoline are in start.S. */

#define RELOC_NONE       0      /* R_X86_64_NONE */
#define RELOC_ABS64      1      /* R_X86_64_64: S + A */
#define RELOC_COPY       5      /* R_X86_64_COPY */
#define RELOC_GLOB_DAT   6      /* R_X86_64_GLOB_DAT: S */
#define RELOC_JUMP_SLOT  7      /* R_X86_64_JUMP_SLOT: S */
#define RELOC_RELATIVE   8      /* R_X86_64_RELATIVE: B + A */
#define RELOC_TLS_DTPMOD 16     /* R_X86_64_DTPMOD64 */
#define RELOC_TLS_DTPREL 17     /* R_X86_64_DTPOFF64 */
#define RELOC_TLS_TPREL  18     /* R_X86_64_TPOFF64 */

static inline long ld_arch_syscall(long nr, long a, long b, long c, long d, long e, long f)
{
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}

/* The thread pointer (the FS base), which addresses the control block. */
static inline void *ld_arch_thread_pointer(void)
{
    void *tcb;
    __asm__ volatile("movq %%fs:0, %0" : "=r"(tcb));
    return tcb;
}

/* The initial-exec offset of a TLS symbol from the thread pointer. In TLS
 * variant II the block of a module lies module_offset bytes below the
 * control block, so the offset is negative. */
static inline uint64_t ld_arch_tls_tprel(uint64_t symbol_offset, uint64_t module_offset)
{
    return symbol_offset - module_offset;
}
