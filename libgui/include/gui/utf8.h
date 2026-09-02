#pragma once
/* Small, strict UTF-8 helpers shared by the text widgets and protocol
 * client. Indices used by libgui are byte offsets at code-point boundaries. */
#include <stdint.h>

uint32_t gui_utf8_decode(const char *s, int len, int *at);
int gui_utf8_encode(uint32_t cp, char out[4]);
int gui_utf8_next_boundary(const char *s, int len, int at);
int gui_utf8_prev_boundary(const char *s, int at);

