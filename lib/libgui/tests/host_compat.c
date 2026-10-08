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

int conf_export_locale(void)
{
    return 0;
}

/* The host tests read no configuration file. */
char *conf_lookup(const char *path, const char *key, char *buf, size_t size)
{
    (void)path;
    (void)key;
    (void)buf;
    (void)size;
    return NULL;
}

const char *conf_user_file(const char *name, const char *default_path, char *buf, size_t size)
{
    strncpy(buf, default_path, size - 1);
    buf[size - 1] = '\0';
    return buf;
}

const char *conf_user_write_file(const char *name, char *buf, size_t size)
{
    strncpy(buf, "/nonexistent/user-file", size - 1);
    buf[size - 1] = '\0';
    return buf;
}

const char *conf_home(void)
{
    return "/nonexistent";
}

/* The sleep and processor calls of minios on the host: one processor. */
#include <time.h>

int sleep_ms(unsigned long ms)
{
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000 };
    return nanosleep(&ts, NULL);
}

int nproc(void)
{
    return 1;
}

int gui_host_getcpu(void)
{
    return 0;
}
