#pragma once
#include <stdint.h>

/* The x86_64 part of the dynamic loader (docs/design/dynlink.md): the
 * relocation types it applies under generic names, the system call
 * instruction and the thread pointer. The TLS layout is in minios/dl.h.
 * The entry and the lazy binding trampoline are in start.S. */

/* The machine of the objects the loader accepts. */
#define LD_ARCH_ELF_MACHINE 62    /* EM_X86_64 */

#define RELOC_NONE       0      /* R_X86_64_NONE */
#define RELOC_ABS64      1      /* R_X86_64_64: S + A */
#define RELOC_COPY       5      /* R_X86_64_COPY */
#define RELOC_GLOB_DAT   6      /* R_X86_64_GLOB_DAT: S */
#define RELOC_JUMP_SLOT  7      /* R_X86_64_JUMP_SLOT: S */
#define RELOC_RELATIVE   8      /* R_X86_64_RELATIVE: B + A */
#define RELOC_TLS_DTPMOD 16     /* R_X86_64_DTPMOD64 */
#define RELOC_TLS_DTPREL 17     /* R_X86_64_DTPOFF64 */
#define RELOC_TLS_TPREL  18     /* R_X86_64_TPOFF64 */

/* Words of the recovery buffer of _dl_setjmp: rbx, rbp, r12 to r15, rsp
 * and the return address. */
#define LD_ARCH_JMPBUF_WORDS 8

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
