#pragma once
#include <stddef.h>
#define GLOB_ERR 1
#define GLOB_MARK 2
#define GLOB_NOSORT 4
#define GLOB_DOOFFS 8
#define GLOB_NOCHECK 16
#define GLOB_APPEND 32
#define GLOB_NOESCAPE 64
#define GLOB_NOSPACE 1
#define GLOB_ABORTED 2
#define GLOB_NOMATCH 3
typedef struct {
    size_t gl_pathc;
    char **gl_pathv;
    size_t gl_offs;
} glob_t;
int glob(const char *pattern, int flags, int (*errfunc)(const char *, int), glob_t *out);
void globfree(glob_t *out);
