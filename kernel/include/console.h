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
/* Replace the active framebuffer description (drivers/fbdev.h) and
 * re-initialize the console for it, under console_lock. */
struct limine_framebuffer;
void console_set_screen(const struct limine_framebuffer *screen, uint32_t scale);
/* Fetch and clear the rectangle the console drew since the last call.
 * Returns false when nothing changed. */
struct fb_rect;
bool console_take_dirty(struct fb_rect *r);
