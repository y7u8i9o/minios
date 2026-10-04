#pragma once
int term_use_color(int fd);
int term_columns(int fd);
/* ANSI SGR parameter (0 resets attributes). */
const char *term_sgr(int color);
