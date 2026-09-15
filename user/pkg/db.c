/* The installer's records under <prefix>/lib/pkg: a lock, and per package
 * the manifest, the owned files with size and CRC-32, and the directories
 * the package created. The launcher and MIME tables the desktop reads are
 * rewritten from the manifests after every change. */
#include "pkg.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <minios/local.h>

const char *prefix = LOCAL_PREFIX;
const char *sysroot = "";           /* --root: the tree holding lib/abi and the system libraries */

void path_join(char *buf, size_t n, const char *dir, const char *rel)
{
    snprintf(buf, n, "%s/%s", dir, rel);
}

int read_file(const char *path, uint8_t **data, size_t *len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    struct stat st;
    if (fstat(fd, &st) < 0) {
        close(fd);
        return -1;
    }
    size_t size = (size_t)st.st_size;
    uint8_t *buf = malloc(size + 1);
    if (!buf) {
        close(fd);
        errno = ENOMEM;
        return -1;
    }
    size_t got = 0;
    while (got < size) {
        ssize_t r = read(fd, buf + got, size - got);
        if (r < 0) {
            free(buf);
            close(fd);
            return -1;
        }
        if (r == 0)
            break;
        got += (size_t)r;
    }
    close(fd);
    buf[got] = '\0';
    *data = buf;
    *len = got;
    return 0;
}

int write_file(const char *path, const uint8_t *data, size_t len)
{
    return write_file_mode(path, data, len, 0644);
}

int write_file_mode(const char *path, const uint8_t *data, size_t len, uint32_t mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0)
        return -1;
    size_t done = 0;
    while (done < len) {
        ssize_t r = write(fd, data + done, len - done);
        if (r < 0) {
            close(fd);
            return -1;
        }
        done += (size_t)r;
    }
    return close(fd);
}

static int mkdir_all(const char *path)
{
    char buf[PKG_PATH_MAX];
    strlcpy(buf, path, sizeof buf);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(buf, 0755) < 0 && errno != EEXIST)
            return -1;
        *p = '/';
    }
    return mkdir(buf, 0755) < 0 && errno != EEXIST ? -1 : 0;
}

static void db_dir(char *buf, size_t n, const char *name)
{
    if (name)
        snprintf(buf, n, "%s/lib/pkg/%s", prefix, name);
    else
        snprintf(buf, n, "%s/lib/pkg", prefix);
}

/* The lock is a file created exclusively with the holder's pid; a lock
 * whose holder no longer exists is taken over. */
int db_lock(void)
{
    char dir[PKG_PATH_MAX], lock[PKG_PATH_MAX];
    db_dir(dir, sizeof dir, NULL);
    if (mkdir_all(dir) < 0)
        return -errno;
    path_join(lock, sizeof lock, dir, "lock");
    for (int attempt = 0; attempt < 2; attempt++) {
        int fd = open(lock, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd >= 0) {
            char pid[16];
            int n = snprintf(pid, sizeof pid, "%d\n", (int)getpid());
            (void)!write(fd, pid, (size_t)n);
            close(fd);
            return 0;
        }
        if (errno != EEXIST)
            return -errno;
        uint8_t *data;
        size_t len;
        if (read_file(lock, &data, &len) < 0)
            return -EBUSY;
        int holder = atoi((const char *)data);
        free(data);
        if (holder > 0 && (kill(holder, 0) == 0 || errno != ESRCH))
            return -EBUSY;
        unlink(lock);
    }
    return -EBUSY;
}

void db_unlock(void)
{
    char lock[PKG_PATH_MAX];
    db_dir(lock, sizeof lock, NULL);
    strlcat(lock, "/lock", sizeof lock);
    unlink(lock);
}

int db_read(const char *name, struct manifest *m)
{
    char path[PKG_PATH_MAX], err[128];
    db_dir(path, sizeof path, name);
    strlcat(path, "/manifest", sizeof path);
    struct stat st;
    if (stat(path, &st) < 0)
        return -errno;
    return manifest_read(m, path, err, sizeof err) < 0 ? -EINVAL : 0;
}

static int cmp_names(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

/* The installed package names, sorted; the count, with *names allocated. */
int db_names(char (**names)[PKG_NAME_MAX])
{
    char dir[PKG_PATH_MAX];
    db_dir(dir, sizeof dir, NULL);
    DIR *d = opendir(dir);
    *names = NULL;
    if (!d)
        return 0;
    int n = 0, cap = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_type != DT_DIR || e->d_name[0] == '.' || !name_valid(e->d_name))
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 16;
            char (*grown)[PKG_NAME_MAX] = realloc(*names, (size_t)cap * PKG_NAME_MAX);
            if (!grown) {
                closedir(d);
                free(*names);
                *names = NULL;
                return -ENOMEM;
            }
            *names = grown;
        }
        strlcpy((*names)[n++], e->d_name, PKG_NAME_MAX);
    }
    closedir(d);
    if (n)
        qsort(*names, (size_t)n, PKG_NAME_MAX, cmp_names);
    return n;
}

int record_add_file(struct record *r, const char *path, size_t size, uint32_t crc)
{
    if (r->nfiles == r->cap_files) {
        int cap = r->cap_files ? r->cap_files * 2 : 32;
        struct owned *f = realloc(r->files, (size_t)cap * sizeof *f);
        if (!f)
            return -ENOMEM;
        r->files = f;
        r->cap_files = cap;
    }
    strlcpy(r->files[r->nfiles].path, path, PKG_PATH_MAX);
    r->files[r->nfiles].size = size;
    r->files[r->nfiles].crc = crc;
    r->nfiles++;
    return 0;
}

int record_has_dir(const struct record *r, const char *path)
{
    for (int i = 0; i < r->ndirs; i++)
        if (strcmp(r->dirs[i], path) == 0)
            return 1;
    return 0;
}

int record_add_dir(struct record *r, const char *path)
{
    if (record_has_dir(r, path))
        return 0;
    if (r->ndirs == r->cap_dirs) {
        int cap = r->cap_dirs ? r->cap_dirs * 2 : 16;
        char (*d)[PKG_PATH_MAX] = realloc(r->dirs, (size_t)cap * PKG_PATH_MAX);
        if (!d)
            return -ENOMEM;
        r->dirs = d;
        r->cap_dirs = cap;
    }
    strlcpy(r->dirs[r->ndirs++], path, PKG_PATH_MAX);
    return 0;
}

void record_free(struct record *r)
{
    free(r->files);
    free(r->dirs);
    memset(r, 0, sizeof *r);
}

int db_read_record(const char *name, struct record *r)
{
    char path[PKG_PATH_MAX];
    uint8_t *data;
    size_t len;
    memset(r, 0, sizeof *r);
    db_dir(path, sizeof path, name);
    strlcat(path, "/files", sizeof path);
    if (read_file(path, &data, &len) < 0)
        return -errno;
    char *line = strtok((char *)data, "\n");
    while (line) {
        char *sp = strrchr(line, ' '), *sp2 = NULL;
        if (sp) {
            *sp = '\0';
            sp2 = strrchr(line, ' ');
            *sp = ' ';
        }
        if (sp && sp2) {
            *sp2 = '\0';
            record_add_file(r, line, (size_t)strtoull(sp2 + 1, NULL, 10), (uint32_t)strtoul(sp + 1, NULL, 16));
        }
        line = strtok(NULL, "\n");
    }
    free(data);
    db_dir(path, sizeof path, name);
    strlcat(path, "/dirs", sizeof path);
    if (read_file(path, &data, &len) == 0) {
        line = strtok((char *)data, "\n");
        while (line) {
            if (*line)
                record_add_dir(r, line);
            line = strtok(NULL, "\n");
        }
        free(data);
    }
    return 0;
}

int db_write(const char *name, const struct manifest *m, const struct record *r)
{
    char dir[PKG_PATH_MAX], path[PKG_PATH_MAX];
    db_dir(dir, sizeof dir, name);
    if (mkdir_all(dir) < 0)
        return -errno;
    path_join(path, sizeof path, dir, "manifest");
    FILE *f = fopen(path, "w");
    if (!f)
        return -errno;
    manifest_write(f, m);
    fclose(f);
    path_join(path, sizeof path, dir, "files");
    f = fopen(path, "w");
    if (!f)
        return -errno;
    for (int i = 0; i < r->nfiles; i++)
        fprintf(f, "%s %zu %08x\n", r->files[i].path, r->files[i].size, (unsigned)r->files[i].crc);
    fclose(f);
    path_join(path, sizeof path, dir, "dirs");
    f = fopen(path, "w");
    if (!f)
        return -errno;
    for (int i = 0; i < r->ndirs; i++)
        fprintf(f, "%s\n", r->dirs[i]);
    fclose(f);
    return 0;
}

int db_delete(const char *name)
{
    char dir[PKG_PATH_MAX], path[PKG_PATH_MAX];
    db_dir(dir, sizeof dir, name);
    static const char *const parts[] = { "manifest", "files", "dirs" };
    for (size_t i = 0; i < sizeof parts / sizeof parts[0]; i++) {
        path_join(path, sizeof path, dir, parts[i]);
        unlink(path);
    }
    return rmdir(dir) < 0 ? -errno : 0;
}

/* The package owning a prefix relative path; 1 with owner set, else 0. */
int db_owner(const char *relpath, char *owner, size_t n)
{
    char (*names)[PKG_NAME_MAX];
    int count = db_names(&names);
    int found = 0;
    for (int i = 0; i < count && !found; i++) {
        struct record r;
        if (db_read_record(names[i], &r) < 0)
            continue;
        for (int j = 0; j < r.nfiles; j++)
            if (strcmp(r.files[j].path, relpath) == 0) {
                strlcpy(owner, names[i], n);
                found = 1;
                break;
            }
        record_free(&r);
    }
    free(names);
    return found;
}

static int write_table(const char *rel, const char *header, void (*emit)(FILE *, const struct manifest *))
{
    char path[PKG_PATH_MAX], tmp[PKG_PATH_MAX];
    snprintf(path, sizeof path, "%s/share/%s", prefix, rel);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f)
        return -errno;
    fprintf(f, "%s\n", header);
    char (*names)[PKG_NAME_MAX];
    int count = db_names(&names);
    for (int i = 0; i < count; i++) {
        struct manifest m;
        if (db_read(names[i], &m) == 0)
            emit(f, &m);
    }
    free(names);
    fclose(f);
    return rename(tmp, path) < 0 ? -errno : 0;
}

static void emit_launcher(FILE *f, const struct manifest *m)
{
    for (int i = 0; i < m->nlaunchers; i++)
        fprintf(f, "%s=%s/%s\n", m->launchers[i].title, prefix, m->launchers[i].command);
}

static void emit_types(FILE *f, const struct manifest *m)
{
    for (int i = 0; i < m->ntypes; i++)
        fprintf(f, "%s %s\n", m->types[i].type, m->types[i].extensions);
}

static void emit_handlers(FILE *f, const struct manifest *m)
{
    for (int i = 0; i < m->nhandlers; i++)
        fprintf(f, "%s %s/%s\n", m->handlers[i].type, prefix, m->handlers[i].command);
}

int db_write_tables(void)
{
    char dir[PKG_PATH_MAX];
    snprintf(dir, sizeof dir, "%s/share", prefix);
    if (mkdir_all(dir) < 0)
        return -errno;
    int r = write_table("launcher", "# Launcher entries of installed packages, written by pkg", emit_launcher);
    if (r == 0)
        r = write_table("mime.types", "# type extensions, written by pkg", emit_types);
    if (r == 0)
        r = write_table("mime.apps", "# type program, written by pkg", emit_handlers);
    return r;
}

/* The ABI number of a system library from /lib/abi, or -1. */
int system_abi(const char *soname)
{
    uint8_t *data;
    size_t len;
    char path[PKG_PATH_MAX];
    snprintf(path, sizeof path, "%s/lib/abi", sysroot);
    if (read_file(path, &data, &len) < 0)
        return -1;
    int abi = -1;
    char *line = strtok((char *)data, "\n");
    while (line && abi < 0) {
        char *sp = strchr(line, ' ');
        if (sp) {
            *sp = '\0';
            if (strcmp(line, soname) == 0)
                abi = atoi(sp + 1);
        }
        line = strtok(NULL, "\n");
    }
    free(data);
    return abi;
}
