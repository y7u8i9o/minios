#pragma once
/* The integer limits of x86_64 and the POSIX limits of minios. gcc has
 * its own limits.h with the integer part; tcc has none, so the values are
 * given here for it. */
#if defined(__GNUC__) && !defined(__TINYC__)
#include_next <limits.h>
#else
#define CHAR_BIT 8
#define SCHAR_MIN (-128)
#define SCHAR_MAX 127
#define UCHAR_MAX 255
#define CHAR_MIN SCHAR_MIN
#define CHAR_MAX SCHAR_MAX
#define MB_LEN_MAX 4
#define SHRT_MIN (-32768)
#define SHRT_MAX 32767
#define USHRT_MAX 65535
#define INT_MIN (-2147483647 - 1)
#define INT_MAX 2147483647
#define UINT_MAX 4294967295U
#define LONG_MIN (-9223372036854775807L - 1)
#define LONG_MAX 9223372036854775807L
#define ULONG_MAX 18446744073709551615UL
#define LLONG_MIN (-9223372036854775807LL - 1)
#define LLONG_MAX 9223372036854775807LL
#define ULLONG_MAX 18446744073709551615ULL
#endif

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
