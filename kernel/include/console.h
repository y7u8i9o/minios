#pragma once
#include <kernel.h>

/* The console writes to the serial port and, once present, the framebuffer
 * console. Before consoleout starts, direct output is serialized by
 * console_lock. Afterwards producers use per-CPU rings and consoleout is the
 * sole serial writer; console_lock then protects framebuffer state only. */
void console_init(void);
/* Switch ordinary output to per-CPU SPSC queues drained by consoleout. */
void console_start_daemon(void);
void console_putc(char c);
void console_write(const char *s, size_t n);
/* Process-context output: ordered and lossless, may sleep. Unlike kernel
 * logging, terminal control sequences must not use per-CPU lossy queues. */
void console_write_user(const char *s, size_t n);
void console_flush(void);
/* Write the queued output of every CPU directly; panic path only. */
void console_panic_drain(void);
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
