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

/* The configuration file of the desktop session; the host tests read
 * nothing from it. */
#include <string.h>
#include "minios/conf.h"

const char *conf_read_path(char *buf, size_t size)
{
    strncpy(buf, "/nonexistent/desktop.conf", size - 1);
    buf[size - 1] = '\0';
    return buf;
}

const char *conf_write_path(char *buf, size_t size)
{
    return conf_read_path(buf, size);
}
