#pragma once

/* x86-64 System V: six callee-saved registers, the post-return stack
 * pointer and the return address.  Keep the representation opaque to
 * callers even though an array type is required by the C interface. */
typedef unsigned long jmp_buf[8];

int setjmp(jmp_buf env) __attribute__((returns_twice));
void longjmp(jmp_buf env, int value) __attribute__((noreturn));
