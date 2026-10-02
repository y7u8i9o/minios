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

/* set_var sets or removes one variable and reports a change. */
static int set_var(const char *name, const char *value)
{
    const char *old = getenv(name);
    if (value[0]) {
        if (old && strcmp(old, value) == 0)
            return 0;
        setenv(name, value, 1);
        return 1;
    }
    if (!old)
        return 0;
    unsetenv(name);
    return 1;
}

int conf_export_locale(void)
{
    char path[256], line[256], lang[64] = "", formats[64] = "";
    FILE *f = fopen(conf_read_path(path, sizeof path), "r");
    if (!f)
        return 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = '\0';
        if (strncmp(line, "lang=", 5) == 0)
            strlcpy(lang, line + 5, sizeof lang);
        else if (strncmp(line, "formats=", 8) == 0)
            strlcpy(formats, line + 8, sizeof formats);
    }
    fclose(f);
    if (!lang[0])
        return 0;
    int changed = set_var("LANG", lang);
    changed |= set_var("LC_NUMERIC", formats);
    changed |= set_var("LC_TIME", formats);
    changed |= set_var("LC_MONETARY", formats);
    return changed;
}
