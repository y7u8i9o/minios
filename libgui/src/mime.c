/* MIME types: extension table and handler table, both plain text with
 * one entry per line. */
#include <gui/mime.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <minios/local.h>
#include <minios/conf.h>

/* local: from the tables of installed packages under the package prefix,
 * which mime_save leaves out. */
struct ext_entry { char type[48]; char ext[16]; int local; };
struct app_entry { char type[48]; char program[64]; int local; };

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

static void load_types(const char *path, int local)
{
    char line[256];
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    while (fgets(line, sizeof line, f)) {
        char *p = trim(line);
        if (!*p || *p == '#') continue;
        char *type = strtok(p, " \t");
        char *ext;
        while (type && (ext = strtok(NULL, " \t")) != NULL && nexts < 128) {
            strlcpy(exts[nexts].type, type, sizeof exts[0].type);
            strlcpy(exts[nexts].ext, ext, sizeof exts[0].ext);
            exts[nexts].local = local;
            nexts++;
        }
    }
    fclose(f);
}

static int find_app(const char *type)
{
    for (int i = 0; i < napps; i++)
        if (strcmp(apps[i].type, type) == 0)
            return i;
    return -1;
}

/* Package entries are added after the system table and only for types
 * the table does not name, so the user's handler table takes precedence
 * on the same type. */
static int load_apps(const char *path, int local)
{
    char line[256];
    FILE *f = fopen(path, "r");
    if (!f)
        return -errno;
    while (fgets(line, sizeof line, f)) {
        char *p = trim(line);
        if (!*p || *p == '#') continue;
        char *type = strtok(p, " \t");
        char *prog = strtok(NULL, " \t");
        if (type && prog && napps < MIME_MAX && !(local && find_app(type) >= 0)) {
            strlcpy(apps[napps].type, type, sizeof apps[0].type);
            strlcpy(apps[napps].program, prog, sizeof apps[0].program);
            apps[napps].local = local;
            napps++;
        }
    }
    fclose(f);
    return 0;
}

int mime_load(const char *types_path, const char *apps_path)
{
    loaded = 1;
    nexts = napps = 0;
    load_types(types_path ? types_path : "/etc/mime.types", 0);
    if (apps_path)
        strlcpy(apps_file, apps_path, sizeof apps_file);
    int r = load_apps(apps_file, 0);
    if (r < 0)
        return r;
    load_types(LOCAL_MIME_TYPES, 1);
    load_apps(LOCAL_MIME_APPS, 1);
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
    apps[i].local = 0;
}

int mime_save(const char *apps_path)
{
    ensure();
    FILE *f = fopen(apps_path ? apps_path : apps_file, "w");
    if (!f)
        return -errno;
    fprintf(f, "# type program\n");
    for (int i = 0; i < napps; i++)
        if (!apps[i].local)
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
    /* Packages may have been installed or removed since this window opened. */
    mime_load(NULL, NULL);
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
    int r = mime_spawn(argv);
    return r < 0 ? r : 1;
}

int mime_spawn(char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0)
        return -errno;
    if (pid == 0) {
        pid_t grandchild = fork();
        if (grandchild == 0) {
            /* The program follows the language of the desktop settings. */
            conf_export_locale();
            execvp(argv[0], argv);
            _exit(127);
        }
        _exit(grandchild < 0 ? 1 : 0);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -EAGAIN;
}
