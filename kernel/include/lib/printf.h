#pragma once
#include <kernel.h>

/* Core formatter: calls emit for every produced character. Returns the number
 * of characters produced. Supports %d %i %u %x %X %p %s %c %% with the l, ll
 * and z length modifiers, the '-' and '0' flags, and a numeric width. */
typedef void (*printf_emit_fn)(char c, void *arg);
int kvformat(printf_emit_fn emit, void *arg, const char *fmt, va_list ap);

int kvsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int ksnprintf(char *buf, size_t size, const char *fmt, ...) __printf(3, 4);
