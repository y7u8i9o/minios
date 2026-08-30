#pragma once
#include <kernel.h>
#include <limine.h>

void fbcon_init(struct limine_framebuffer *fb);
bool fbcon_present(void);
/* Text size in cells; 80x25 when no framebuffer exists. */
void fbcon_get_size(uint16_t *cols, uint16_t *rows);
/* Stop or resume drawing; resuming redraws the text from the cell buffer.
 * Output while disabled still updates the cells. */
void fbcon_set_enabled(bool enabled);
void fbcon_putc(char c);
void fbcon_write(const char *s, size_t n);
