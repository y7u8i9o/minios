#pragma once
#include <kernel.h>

/* The console writes to the serial port and, once present, the framebuffer
 * console. Output is serialized by console_lock. */
void console_init(void);
void console_putc(char c);
void console_write(const char *s, size_t n);
int kprintf(const char *fmt, ...) __printf(1, 2);
int kvprintf(const char *fmt, va_list ap);
/* Hand the framebuffer to user space or take it back, under console_lock. */
void console_set_fb_enabled(bool enabled);
