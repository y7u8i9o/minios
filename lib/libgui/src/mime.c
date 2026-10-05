/* MIME types: extension table and handler table, both plain text with
 * one entry per line. */
#include <gui/mime.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <minios/local.h>
#include <minios/conf.h>

/* local: from the tables of installed packages (PKG_MIME_TYPES and
 * PKG_MIME_APPS), which mime_save leaves out.  The program of a handler
 * is a command: a program followed by arguments separated by spaces. */
struct ext_entry { char type[48]; char ext[16]; int local; };
struct app_entry { char type[48]; char program[MIME_COMMAND]; int local; };

static struct ext_entry exts[128];
static int nexts;
static struct app_entry apps[MIME_MAX];
static int napps;
/* The package handlers for types with an entry in the system or user
 * table.  mime_handler ignores them, and mime_handlers lists them. */
static struct app_entry shadowed[MIME_MAX];
static int nshadowed;
static int loaded;
/* The handler table: the user's ~/.config/mime.apps when it exists, else
 * /etc/mime.apps, unless mime_load receives a path. mime_save writes to the
 * user's file in the first case (docs/design/users.md). */
static char apps_file[256];
static int apps_named;
/* The extension table: /etc/mime.types unless mime_load receives a path. */
static char types_file[256] = "/etc/mime.types";

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
        char *prog = strtok(NULL, "");
        if (!type || !prog || !*(prog = trim(prog)))
            continue;
        struct app_entry *e = NULL;
        if (local && find_app(type) >= 0) {
            if (nshadowed < MIME_MAX)
                e = &shadowed[nshadowed++];
        } else if (napps < MIME_MAX) {
            e = &apps[napps++];
        }
        if (e) {
            strlcpy(e->type, type, sizeof e->type);
            strlcpy(e->program, prog, sizeof e->program);
            e->local = local;
        }
    }
    fclose(f);
    return 0;
}

int mime_load(const char *types_path, const char *apps_path)
{
    loaded = 1;
    nexts = napps = nshadowed = 0;
    if (types_path)
        strlcpy(types_file, types_path, sizeof types_file);
    load_types(types_file, 0);
    if (apps_path) {
        strlcpy(apps_file, apps_path, sizeof apps_file);
        apps_named = 1;
    } else if (!apps_named) {
        conf_user_file("mime.apps", "/etc/mime.apps", apps_file, sizeof apps_file);
    }
    int r = load_apps(apps_file, 0);
    if (r < 0)
        return r;
    load_types(PKG_MIME_TYPES, 1);
    load_apps(PKG_MIME_APPS, 1);
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

/* add_handler appends program to list unless list contains it. */
static int add_handler(const char **list, int n, int max, const char *program)
{
    for (int i = 0; i < n; i++)
        if (strcmp(list[i], program) == 0)
            return n;
    if (n < max)
        list[n++] = program;
    return n;
}

int mime_handlers(const char *type, const char **list, int max)
{
    ensure();
    char wild[48] = "";
    const char *slash = strchr(type, '/');
    if (slash)
        snprintf(wild, sizeof wild, "%.*s/*", (int)(slash - type), type);
    const char *keys[3] = { type, wild, "*" };
    int n = 0;
    for (int k = 0; k < 3; k++) {
        if (!keys[k][0])
            continue;
        for (int i = 0; i < napps; i++)
            if (strcmp(apps[i].type, keys[k]) == 0)
                n = add_handler(list, n, max, apps[i].program);
        for (int i = 0; i < nshadowed; i++)
            if (strcmp(shadowed[i].type, keys[k]) == 0)
                n = add_handler(list, n, max, shadowed[i].program);
    }
    return n;
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
    char user_file[256];
    if (!apps_path)
        apps_path = apps_named ? apps_file : conf_user_write_file("mime.apps", user_file, sizeof user_file);
    FILE *f = fopen(apps_path, "w");
    if (!f)
        return -errno;
    fprintf(f, "# type command\n");
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
    if (strcmp(type, MIME_LAUNCHER) == 0) {
        int r = launcher_command(path, cmd, sizeof cmd);
        if (r < 0)
            return r;
        r = mime_run(cmd, NULL);
        return r < 0 ? r : 1;
    }
    const char *prog = mime_handler(type);
    if (!prog)
        return -ENOENT;
    int r = mime_run(prog, path);
    return r < 0 ? r : 1;
}

#define RUN_ARGS 16

int mime_run(const char *command, const char *path)
{
    char line[MIME_COMMAND], *argv[RUN_ARGS + 2], *save;
    int n = 0;
    strlcpy(line, command, sizeof line);
    for (char *w = strtok_r(line, " ", &save); w && n < RUN_ARGS; w = strtok_r(NULL, " ", &save))
        argv[n++] = w;
    if (n == 0)
        return -EINVAL;
    if (path)
        argv[n++] = (char *)path;
    argv[n] = NULL;
    return mime_spawn(argv);
}

/* The grandchild reports a failed exec through a pipe.  The write end of
 * the pipe has FD_CLOEXEC.  A successful exec closes the write end, and
 * the parent reads end of file. */
int mime_spawn(char *const argv[])
{
    int fds[2];
    if (pipe(fds) < 0)
        return -errno;
    if (fcntl(fds[1], F_SETFD, FD_CLOEXEC) < 0) {
        int err = errno;
        close(fds[0]);
        close(fds[1]);
        return -err;
    }
    pid_t pid = fork();
    if (pid < 0) {
        int err = errno;
        close(fds[0]);
        close(fds[1]);
        return -err;
    }
    if (pid == 0) {
        close(fds[0]);
        pid_t grandchild = fork();
        if (grandchild == 0) {
            /* The program follows the language of the desktop settings. */
            conf_export_locale();
            execvp(argv[0], argv);
            int err = errno;
            ssize_t ignored = write(fds[1], &err, sizeof err);
            (void)ignored;
            _exit(127);
        }
        if (grandchild < 0) {
            int err = errno;
            ssize_t ignored = write(fds[1], &err, sizeof err);
            (void)ignored;
        }
        _exit(0);
    }
    close(fds[1]);
    int err = 0;
    ssize_t got;
    while ((got = read(fds[0], &err, sizeof err)) < 0 && errno == EINTR)
        ;
    close(fds[0]);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
        ;
    if (got == (ssize_t)sizeof err)
        return err > 0 ? -err : -EAGAIN;
    return got < 0 ? -EIO : 0;
}
