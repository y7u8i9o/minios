/* MIME types: extension table and handler table, both plain text with
 * one entry per line. */
#include <gui/mime.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct ext_entry { char type[48]; char ext[16]; };
struct app_entry { char type[48]; char program[64]; };

static struct ext_entry exts[128];
static int nexts;
static struct app_entry apps[MIME_MAX];
static int napps;
static int loaded;
static char apps_file[128] = "/etc/mime.apps";

static int ext_equal(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        int ca = *a >= 'A' && *a <= 'Z' ? *a + 32 : *a, cb = *b >= 'A' && *b <= 'Z' ? *b + 32 : *b;
        if (ca != cb)
            return 0;
    }
    return *a == *b;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\n' || e[-1] == '\r')) *--e = '\0';
    return s;
}

int mime_load(const char *types_path, const char *apps_path)
{
    loaded = 1;
    nexts = napps = 0;
    char line[256];
    FILE *f = fopen(types_path ? types_path : "/etc/mime.types", "r");
    if (f) {
        while (fgets(line, sizeof line, f)) {
            char *p = trim(line);
            if (!*p || *p == '#') continue;
            char *type = strtok(p, " \t");
            char *ext;
            while (type && (ext = strtok(NULL, " \t")) != NULL && nexts < 128) {
                strlcpy(exts[nexts].type, type, sizeof exts[0].type);
                strlcpy(exts[nexts].ext, ext, sizeof exts[0].ext);
                nexts++;
            }
        }
        fclose(f);
    }
    if (apps_path)
        strlcpy(apps_file, apps_path, sizeof apps_file);
    f = fopen(apps_file, "r");
    if (!f)
        return -errno;
    while (fgets(line, sizeof line, f)) {
        char *p = trim(line);
        if (!*p || *p == '#') continue;
        char *type = strtok(p, " \t");
        char *prog = strtok(NULL, " \t");
        if (type && prog && napps < MIME_MAX) {
            strlcpy(apps[napps].type, type, sizeof apps[0].type);
            strlcpy(apps[napps].program, prog, sizeof apps[0].program);
            napps++;
        }
    }
    fclose(f);
    return 0;
}

static void ensure(void)
{
    if (!loaded)
        mime_load(NULL, NULL);
}

const char *mime_type(const char *path, int is_dir)
{
    ensure();
    if (is_dir)
        return MIME_DIRECTORY;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *dot = strrchr(base, '.');
    if (dot && dot != base) {
        for (int i = 0; i < nexts; i++)
            if (ext_equal(exts[i].ext, dot + 1))
                return exts[i].type;
    }
    return "application/octet-stream";
}

static int find_app(const char *type)
{
    for (int i = 0; i < napps; i++)
        if (strcmp(apps[i].type, type) == 0)
            return i;
    return -1;
}

const char *mime_handler(const char *type)
{
    ensure();
    int i = find_app(type);
    if (i < 0) {
        char wild[48];
        const char *slash = strchr(type, '/');
        if (slash) {
            snprintf(wild, sizeof wild, "%.*s/*", (int)(slash - type), type);
            i = find_app(wild);
        }
    }
    if (i < 0)
        i = find_app("*");
    return i < 0 ? NULL : apps[i].program;
}

void mime_set_handler(const char *type, const char *program)
{
    ensure();
    int i = find_app(type);
    if (i < 0) {
        if (napps >= MIME_MAX)
            return;
        i = napps++;
        strlcpy(apps[i].type, type, sizeof apps[0].type);
    }
    strlcpy(apps[i].program, program, sizeof apps[0].program);
}

int mime_save(const char *apps_path)
{
    ensure();
    FILE *f = fopen(apps_path ? apps_path : apps_file, "w");
    if (!f)
        return -errno;
    fprintf(f, "# type program\n");
    for (int i = 0; i < napps; i++)
        fprintf(f, "%s %s\n", apps[i].type, apps[i].program);
    fclose(f);
    return 0;
}

int mime_handler_count(void) { ensure(); return napps; }
const char *mime_handler_type(int index) { return index >= 0 && index < napps ? apps[index].type : NULL; }
const char *mime_handler_program(int index) { return index >= 0 && index < napps ? apps[index].program : NULL; }

const char *mime_icon(const char *type)
{
    if (strcmp(type, MIME_DIRECTORY) == 0) return "folder";
    if (strcmp(type, MIME_LAUNCHER) == 0) return "open";
    if (strncmp(type, "text/", 5) == 0) return "edit";
    if (strncmp(type, "image/", 6) == 0) return "paint";
    return "file";
}

/* exec= line of a launcher file, in buf. */
static int launcher_command(const char *path, char *buf, size_t size)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -errno;
    char line[160];
    int found = -ENOENT;
    while (fgets(line, sizeof line, f)) {
        char *p = trim(line);
        if (strncmp(p, "exec=", 5) == 0) {
            strlcpy(buf, p + 5, size);
            found = 0;
            break;
        }
    }
    fclose(f);
    return found;
}

pid_t mime_open(const char *path)
{
    ensure();
    const char *type = mime_type(path, 0);
    char cmd[160];
    char *argv[4];
    if (strcmp(type, MIME_LAUNCHER) == 0) {
        int r = launcher_command(path, cmd, sizeof cmd);
        if (r < 0)
            return r;
        argv[0] = strtok(cmd, " ");
        argv[1] = strtok(NULL, " ");
        argv[2] = NULL;
    } else {
        const char *prog = mime_handler(type);
        if (!prog)
            return -ENOENT;
        strlcpy(cmd, prog, sizeof cmd);
        argv[0] = cmd;
        argv[1] = (char *)path;
        argv[2] = NULL;
    }
    pid_t pid = fork();
    if (pid < 0)
        return -errno;
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    return pid;
}
