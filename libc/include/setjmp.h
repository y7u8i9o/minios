#pragma once

/* jmp_buf is defined by the architecture (bits/<arch>/setjmp.h). */
#if defined(__x86_64__)
#include <bits/x86_64/setjmp.h>
#elif defined(__aarch64__)
#include <bits/aarch64/setjmp.h>
#else
#error "setjmp.h: unsupported architecture"
#endif

int setjmp(jmp_buf env) __attribute__((returns_twice));
void longjmp(jmp_buf env, int value) __attribute__((noreturn));

/* The POSIX pair; identical here because no signal mask is saved. */
int _setjmp(jmp_buf env) __attribute__((returns_twice));
void _longjmp(jmp_buf env, int value) __attribute__((noreturn));
