#pragma once
#include <stddef.h>

/* Stack allocation that ends with the calling function. */
#define alloca(size) __builtin_alloca(size)
