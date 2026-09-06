#pragma once
/* The integer limits come from the compiler's own limits.h; this file adds
 * the POSIX limits of minios. */
#include_next <limits.h>

#ifndef NAME_MAX
#define NAME_MAX 255
#endif
#define PATH_MAX 1024
#define LINE_MAX 2048
#define _POSIX2_LINE_MAX 2048
#define ARG_MAX 65536
#define OPEN_MAX 64
#define PIPE_BUF 4096
#define SSIZE_MAX LONG_MAX
