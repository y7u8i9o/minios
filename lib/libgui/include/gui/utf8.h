#pragma once
/* Small, strict UTF-8 helpers shared by the text widgets and protocol
 * client. Indices used by libgui are byte offsets at code-point boundaries. */
#include <stdint.h>

uint32_t gui_utf8_decode(const char *s, int len, int *at);
int gui_utf8_encode(uint32_t cp, char out[4]);
int gui_utf8_next_boundary(const char *s, int len, int at);
int gui_utf8_prev_boundary(const char *s, int at);

/* Words for word movement and word selection. A word is a run of ASCII
 * letters, digits, '_' and non-ASCII bytes. gui_word_left returns the
 * start of the word before the byte offset at, after spaces. A
 * punctuation character before at counts as one word. gui_word_right
 * returns the end of the word after at and of the spaces that follow it.
 * gui_word_at stores the bounds of the word at at in *start and *end, or
 * of the character at at when it is no word character. */
int gui_word_left(const char *s, int at);
int gui_word_right(const char *s, int len, int at);
void gui_word_at(const char *s, int len, int at, int *start, int *end);

