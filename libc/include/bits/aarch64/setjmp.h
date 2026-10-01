#pragma once

/* AArch64: x19 to x30, sp and d8 to d15 (21 words), rounded to an even
 * number of words. Keep the representation opaque to callers even though
 * an array type is required by the C interface. */
typedef unsigned long jmp_buf[22];
