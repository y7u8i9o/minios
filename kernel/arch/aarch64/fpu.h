#pragma once

/* FP and SIMD state (fpu.S): q0 to q31, FPSR and FPCR. */
#define FPU_AREA_SIZE 528

void fpu_save(void *area);
void fpu_restore(const void *area);
