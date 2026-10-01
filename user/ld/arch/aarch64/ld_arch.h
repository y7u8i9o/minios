#pragma once
#include <stdint.h>

/* The aarch64 part of the dynamic loader (docs/design/dynlink.md): the
 * relocation types it applies under generic names, the system call
 * instruction and the thread pointer. The TLS layout is in minios/dl.h.
 * The entry and the lazy binding trampoline are in start.S. Programs are
 * compiled with -mtls-dialect=trad, so no TLS descriptors occur. */

/* The machine of the objects the loader accepts. */
#define LD_ARCH_ELF_MACHINE 183    /* EM_AARCH64 */

#define RELOC_NONE       0      /* R_AARCH64_NONE */
#define RELOC_ABS64      257    /* R_AARCH64_ABS64: S + A */
#define RELOC_COPY       1024   /* R_AARCH64_COPY */
#define RELOC_GLOB_DAT   1025   /* R_AARCH64_GLOB_DAT: S + A */
#define RELOC_JUMP_SLOT  1026   /* R_AARCH64_JUMP_SLOT: S + A */
#define RELOC_RELATIVE   1027   /* R_AARCH64_RELATIVE: B + A */
#define RELOC_TLS_DTPMOD 1028   /* R_AARCH64_TLS_DTPMOD64 */
#define RELOC_TLS_DTPREL 1029   /* R_AARCH64_TLS_DTPREL64 */
#define RELOC_TLS_TPREL  1030   /* R_AARCH64_TLS_TPREL64 */

/* Words of the recovery buffer of _dl_setjmp: x19 to x30 and sp. The
 * loader is compiled with -mgeneral-regs-only, so no FP register needs
 * saving. */
#define LD_ARCH_JMPBUF_WORDS 14

static inline long ld_arch_syscall(long nr, long a, long b, long c, long d, long e, long f)
{
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e;
    register long x5 __asm__("x5") = f;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5)
                     : "memory");
    return x0;
}

/* The thread pointer (TPIDR_EL0), which addresses the control block. */
static inline void *ld_arch_thread_pointer(void)
{
    void *tcb;
    __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tcb));
    return tcb;
}
