#pragma once
#include <sys/types.h>
#include <limits.h>

#define MAXPATHLEN PATH_MAX
#define NBBY 8
#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define MAX(a, b) (((a) > (b)) ? (a) : (b))
#define nitems(x) (sizeof(x) / sizeof((x)[0]))
#define howmany(x, y) (((x) + ((y) - 1)) / (y))
#define roundup(x, y) ((((x) + ((y) - 1)) / (y)) * (y))
#define rounddown(x, y) (((x) / (y)) * (y))
#define powerof2(x) ((((x) - 1) & (x)) == 0)
