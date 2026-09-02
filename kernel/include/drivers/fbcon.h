#pragma once
#include <kernel.h>
#include <limine.h>
#include <minios/abi.h>

/* Start drawing on fb_screen (drivers/fbdev.h). */
void fbcon_init(void);
/* fb_screen changed (buffer, geometry or scale): recompute the cell grid
 * and redraw. Called under console_lock. */
void fbcon_screen_changed(void);
/* Fetch and clear the rectangle drawn since the last call. Under console_lock. */
struct fb_rect;
bool fbcon_take_dirty(struct fb_rect *r);
bool fbcon_present(void);
/* Text size in cells; 80x25 when no framebuffer exists. */
void fbcon_get_size(uint16_t *cols, uint16_t *rows);
/* Stop or resume drawing; resuming redraws the text from the cell buffer.
 * Output while disabled still updates the cells. */
void fbcon_set_enabled(bool enabled);
void fbcon_putc(char c);
void fbcon_write(const char *s, size_t n);
