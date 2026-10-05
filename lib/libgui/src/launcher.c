/* The launcher tables of the panel, the settings program and the
 * application chooser (gui/launcher.h). */
#include <gui/launcher.h>
#include <libintl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <minios/conf.h>
#include <minios/local.h>

int launcher_read_table(const char *path, int translate, struct launcher_entry *entries, int count, int max)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return count;
    char line[LAUNCHER_TITLE + LAUNCHER_COMMAND + 2];
    while (count < max && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = '\0';
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq)
            continue;
        *eq = '\0';
        struct launcher_entry *e = &entries[count++];
        strlcpy(e->title, translate ? dgettext("launcher", line) : line, sizeof e->title);
        strlcpy(e->command, eq + 1, sizeof e->command);
    }
    fclose(f);
    return count;
}

static int compare_titles(const void *a, const void *b)
{
    const struct launcher_entry *x = a, *y = b;
    return strcasecmp(x->title, y->title);
}

int launcher_read_apps(struct launcher_entry *entries, int max)
{
    char path[300];
    int n = launcher_read_table(PKG_LAUNCHER, 1, entries, 0, max);
    snprintf(path, sizeof path, "%s/.local/share/launcher", conf_home());
    n = launcher_read_table(path, 1, entries, n, max);
    qsort(entries, (size_t)n, sizeof entries[0], compare_titles);
    return n;
}

const char *launcher_system_path(char *buf, int size)
{
    return conf_user_file("launcher", LAUNCHER_SYSTEM, buf, (size_t)size);
}

const char *launcher_program(const char *command, char *buf, int size)
{
    strlcpy(buf, command, (size_t)size);
    buf[strcspn(buf, " ")] = '\0';
    return buf;
}
