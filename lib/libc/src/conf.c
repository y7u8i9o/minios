#include <minios/conf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>

#define DEFAULT_CONF "/etc/desktop.conf"

const char *conf_home(void)
{
    static char dir[256];
    const char *home = getenv("HOME");
    if (home != NULL && home[0] == '/')
        return home;
    if (!dir[0]) {
        struct passwd *pw = getpwuid(getuid());
        snprintf(dir, sizeof dir, "%s", pw && pw->pw_dir[0] == '/' ? pw->pw_dir : "/");
    }
    return dir;
}

static const char *home_dir(void)
{
    return conf_home();
}

const char *conf_user_file(const char *name, const char *default_path, char *buf, size_t size)
{
    snprintf(buf, size, "%s/.config/%s", home_dir(), name);
    struct stat st;
    if (stat(buf, &st) == 0 && S_ISREG(st.st_mode))
        return buf;
    strlcpy(buf, default_path, size);
    return buf;
}

const char *conf_user_write_file(const char *name, char *buf, size_t size)
{
    snprintf(buf, size, "%s/.config", home_dir());
    mkdir(buf, 0755);
    snprintf(buf, size, "%s/.config/%s", home_dir(), name);
    return buf;
}

const char *conf_read_path(char *buf, size_t size)
{
    return conf_user_file("desktop.conf", DEFAULT_CONF, buf, size);
}

const char *conf_write_path(char *buf, size_t size)
{
    return conf_user_write_file("desktop.conf", buf, size);
}

char *conf_lookup(const char *path, const char *key, char *buf, size_t size)
{
    if (size)
        buf[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    char line[512];
    size_t len = strlen(key);
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = '\0';
        if (strncmp(line, key, len) == 0 && line[len] == '=') {
            strlcpy(buf, line + len + 1, size);
            found = 1;
        }
    }
    fclose(f);
    return found ? buf : NULL;
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
    char path[256], lang[64], formats[64];
    conf_read_path(path, sizeof path);
    conf_lookup(path, "lang", lang, sizeof lang);
    conf_lookup(path, "formats", formats, sizeof formats);
    if (!lang[0])
        return 0;
    int changed = set_var("LANG", lang);
    changed |= set_var("LC_NUMERIC", formats);
    changed |= set_var("LC_TIME", formats);
    changed |= set_var("LC_MONETARY", formats);
    return changed;
}
