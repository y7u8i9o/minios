#include <minios/conf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define DEFAULT_CONF "/etc/desktop.conf"

static const char *home_dir(void)
{
    const char *home = getenv("HOME");
    return home != NULL && home[0] == '/' ? home : "/home";
}

const char *conf_read_path(char *buf, size_t size)
{
    snprintf(buf, size, "%s/.config/desktop.conf", home_dir());
    struct stat st;
    if (stat(buf, &st) == 0 && S_ISREG(st.st_mode))
        return buf;
    strlcpy(buf, DEFAULT_CONF, size);
    return buf;
}

const char *conf_write_path(char *buf, size_t size)
{
    snprintf(buf, size, "%s/.config", home_dir());
    mkdir(buf, 0755);
    snprintf(buf, size, "%s/.config/desktop.conf", home_dir());
    return buf;
}
