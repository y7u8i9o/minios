#pragma once

#include <stddef.h>
#include <string.h>

/* Use one implementation on every host. Include the native declaration
 * first, then replace its name: Darwin may define strlcpy as a fortified
 * macro, and defining a function with that macro active breaks compilation. */
#undef strlcpy
#define strlcpy gui_host_strlcpy
size_t gui_host_strlcpy(char *dst, const char *src, size_t size);
