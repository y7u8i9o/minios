#include "host_compat.h"

#include <string.h>

/* Match MiniOS strlcpy even on hosts which do not provide the function. */
size_t gui_host_strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size) {
        size_t n = len < size - 1 ? len : size - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return len;
}
