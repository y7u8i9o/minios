#pragma once
/* User space FPU and SSE state (M23): saved with fxsave into a 512 byte
 * 16 byte aligned area per thread on every switch. The kernel itself
 * never uses these registers. */
#include <kernel.h>

#define FPU_AREA_SIZE 512

static inline void fpu_save(void *area)
{
    __asm__ volatile("fxsave64 (%0)" : : "r"(area) : "memory");
}

static inline void fpu_restore(const void *area)
{
    __asm__ volatile("fxrstor64 (%0)" : : "r"(area) : "memory");
}

/* Fill an area with the initial state (fninit, MXCSR 0x1f80). */
void fpu_init_state(void *area);
/* Enable fxsave and SSE on the calling CPU. */
void fpu_init_cpu(void);
