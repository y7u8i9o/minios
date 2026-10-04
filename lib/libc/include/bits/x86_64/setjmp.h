#pragma once

/* x86-64 System V: six callee-saved registers, the post-return stack
 * pointer and the return address.  Treat the representation as opaque to
 * callers even though an array type is required by the C interface. */
typedef unsigned long jmp_buf[8];
