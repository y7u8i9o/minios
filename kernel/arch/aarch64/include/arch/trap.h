#pragma once
#include <kernel.h>

/* Register state saved by the exception vectors (vectors.S). The layout
 * must match the stores there; the size is a multiple of 16. */
struct trapframe {
    uint64_t x[31];             /* x0 to x30; x29 is the frame pointer, x30 the link register */
    uint64_t sp;                /* SP_EL0 for an entry from EL0, the interrupted SP_EL1 otherwise */
    uint64_t pc;                /* ELR_EL1 */
    uint64_t pstate;            /* SPSR_EL1 */
    uint64_t esr;               /* ESR_EL1 */
    uint64_t far;               /* FAR_EL1 */
    uint64_t kind;              /* vector: TRAP_SYNC, TRAP_IRQ, TRAP_FIQ or TRAP_SERROR, plus TRAP_LOWER */
    uint64_t pad;
};

#define TRAP_SYNC   0
#define TRAP_IRQ    1
#define TRAP_FIQ    2
#define TRAP_SERROR 3
#define TRAP_LOWER  4           /* taken from EL0 */

/* Exception classes of ESR_EL1 (bits 31:26) used by the kernel. */
#define ESR_EC(esr)          (((esr) >> 26) & 0x3f)
#define EC_UNKNOWN           0x00
#define EC_SVC64             0x15
#define EC_IABT_LOWER        0x20
#define EC_IABT_CUR          0x21
#define EC_DABT_LOWER        0x24
#define EC_DABT_CUR          0x25
#define EC_BRK64             0x3c

void trap_dispatch(struct trapframe *tf);
void trap_dump_frame(const struct trapframe *tf);
/* System registers of a kernel mode fault: ESR, FAR, ELR, the translation
 * table bases and the control registers. */
void trap_dump_extra(const struct trapframe *tf);
