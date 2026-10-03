/* The installer's records under <root>/var/lib/pkg: a lock, and per
 * package the manifest, the owned files with mode, owner, size and
 * SHA-256, and the directories the package created. The launcher and MIME
 * tables the desktop reads are rewritten from the manifests after every
 * change. */
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
#include <minios/sha2.h>

/* --root: the installation root, "" for the running system. Files,
 * records, configuration and keys are all below it. */
const char *root = "";
/* --arch: the machine packages must be built for, NULL for the running one. */
const char *target_arch;

void path_join(char *buf, size_t n, const char *dir, const char *rel)
{
    snprintf(buf, n, "%s/%s", dir, rel);
}

/* The path of a root relative name in the installation root. */
void root_path(char *buf, size_t n, const char *rel)
{
    path_join(buf, n, root, rel);
}

/* The path of a name relative to the records directory. */
void db_path(char *buf, size_t n, const char *rel)
{
    snprintf(buf, n, "%s%s/%s", root, PKG_DB, rel);
}

int file_sha256(const char *path, uint8_t sha256[32])
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    struct sha256_ctx c;
    sha256_init(&c);
    uint8_t buf[8192];
    ssize_t r;
    while ((r = read(fd, buf, sizeof buf)) > 0)
        sha256_update(&c, buf, (size_t)r);
    close(fd);
    if (r < 0)
        return -1;
    sha256_final(&c, sha256);
    return 0;
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

int mkdir_all(const char *path)
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
        snprintf(buf, n, "%s%s/%s", root, PKG_DB, name);
    else
        snprintf(buf, n, "%s%s", root, PKG_DB);
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

int record_add_file(struct record *r, const char *path, uint32_t mode, uint32_t uid, uint32_t gid,
                    size_t size, const uint8_t sha256[32])
{
    if (r->nfiles == r->cap_files) {
        int cap = r->cap_files ? r->cap_files * 2 : 32;
        struct owned *f = realloc(r->files, (size_t)cap * sizeof *f);
        if (!f)
            return -ENOMEM;
        r->files = f;
        r->cap_files = cap;
    }
    struct owned *o = &r->files[r->nfiles++];
    strlcpy(o->path, path, PKG_PATH_MAX);
    o->mode = mode;
    o->uid = uid;
    o->gid = gid;
    o->size = size;
    memcpy(o->sha256, sha256, sizeof o->sha256);
    return 0;
}

const struct owned *record_file(const struct record *r, const char *path)
{
    for (int i = 0; i < r->nfiles; i++)
        if (strcmp(r->files[i].path, path) == 0)
            return &r->files[i];
    return NULL;
}

int record_has_dir(const struct record *r, const char *path)
{
    for (int i = 0; i < r->ndirs; i++)
        if (strcmp(r->dirs[i].path, path) == 0)
            return 1;
    return 0;
}

int record_add_dir(struct record *r, const char *path, uint32_t mode, uint32_t uid, uint32_t gid)
{
    if (record_has_dir(r, path))
        return 0;
    if (r->ndirs == r->cap_dirs) {
        int cap = r->cap_dirs ? r->cap_dirs * 2 : 16;
        struct owned *d = realloc(r->dirs, (size_t)cap * sizeof *d);
        if (!d)
            return -ENOMEM;
        r->dirs = d;
        r->cap_dirs = cap;
    }
    struct owned *o = &r->dirs[r->ndirs++];
    memset(o, 0, sizeof *o);
    strlcpy(o->path, path, PKG_PATH_MAX);
    o->mode = mode;
    o->uid = uid;
    o->gid = gid;
    return 0;
}

void record_free(struct record *r)
{
    free(r->files);
    free(r->dirs);
    memset(r, 0, sizeof *r);
}

static int hex_digest(const char *hex, uint8_t out[32])
{
    if (strlen(hex) != 64)
        return -1;
    for (int i = 0; i < 32; i++) {
        unsigned v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1)
            return -1;
        out[i] = (uint8_t)v;
    }
    return 0;
}

/* A line of the file record is "mode uid gid size sha256 path", and a line
 * of the directory record "mode uid gid path". The path comes last since
 * it may contain spaces. */
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
        unsigned mode, uid, gid;
        size_t size;
        char hex[65];
        int at = 0;
        uint8_t digest[32];
        if (sscanf(line, "%o %u %u %zu %64s %n", &mode, &uid, &gid, &size, hex, &at) == 5 && at &&
            line[at] && hex_digest(hex, digest) == 0)
            record_add_file(r, line + at, mode, uid, gid, size, digest);
        line = strtok(NULL, "\n");
    }
    free(data);
    db_dir(path, sizeof path, name);
    strlcat(path, "/dirs", sizeof path);
    if (read_file(path, &data, &len) == 0) {
        line = strtok((char *)data, "\n");
        while (line) {
            unsigned mode, uid, gid;
            int at = 0;
            if (sscanf(line, "%o %u %u %n", &mode, &uid, &gid, &at) == 3 && at && line[at])
                record_add_dir(r, line + at, mode, uid, gid);
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
    for (int i = 0; i < r->nfiles; i++) {
        const struct owned *o = &r->files[i];
        fprintf(f, "%04o %u %u %zu ", (unsigned)o->mode, (unsigned)o->uid, (unsigned)o->gid, o->size);
        for (int j = 0; j < 32; j++)
            fprintf(f, "%02x", o->sha256[j]);
        fprintf(f, " %s\n", o->path);
    }
    fclose(f);
    path_join(path, sizeof path, dir, "dirs");
    f = fopen(path, "w");
    if (!f)
        return -errno;
    for (int i = 0; i < r->ndirs; i++)
        fprintf(f, "%04o %u %u %s\n", (unsigned)r->dirs[i].mode, (unsigned)r->dirs[i].uid,
                (unsigned)r->dirs[i].gid, r->dirs[i].path);
    fclose(f);
    db_owner_reset();
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
    db_owner_reset();
    return rmdir(dir) < 0 ? -errno : 0;
}

/* The owners of all recorded files, sorted by path, built on the first
 * lookup and dropped whenever a record changes. A base system holds
 * thousands of files, and every member of every archive is looked up. */
struct owner_entry { char *path; int pkg; };
static struct owner_entry *owners;
static int nowners = -1;
static char (*owner_names)[PKG_NAME_MAX];

static int cmp_owner(const void *a, const void *b)
{
    return strcmp(((const struct owner_entry *)a)->path, ((const struct owner_entry *)b)->path);
}

void db_owner_reset(void)
{
    for (int i = 0; i < nowners; i++)
        free(owners[i].path);
    free(owners);
    free(owner_names);
    owners = NULL;
    owner_names = NULL;
    nowners = -1;
}

static void load_owners(void)
{
    int count = db_names(&owner_names);
    int cap = 0;
    nowners = 0;
    for (int i = 0; i < count; i++) {
        struct record r;
        if (db_read_record(owner_names[i], &r) < 0)
            continue;
        for (int j = 0; j < r.nfiles; j++) {
            if (nowners == cap) {
                cap = cap ? cap * 2 : 256;
                struct owner_entry *grown = realloc(owners, (size_t)cap * sizeof *owners);
                if (!grown)
                    break;
                owners = grown;
            }
            owners[nowners].path = strdup(r.files[j].path);
            owners[nowners].pkg = i;
            if (owners[nowners].path)
                nowners++;
        }
        record_free(&r);
    }
    if (nowners)
        qsort(owners, (size_t)nowners, sizeof *owners, cmp_owner);
}

/* The package owning a root relative path; 1 with owner set, else 0. */
int db_owner(const char *relpath, char *owner, size_t n)
{
    if (nowners < 0)
        load_owners();
    struct owner_entry key = { (char *)relpath, 0 };
    const struct owner_entry *e = nowners ? bsearch(&key, owners, (size_t)nowners, sizeof *owners, cmp_owner) : NULL;
    if (!e)
        return 0;
    strlcpy(owner, owner_names[e->pkg], n);
    return 1;
}

static int write_table(const char *rel, const char *header, void (*emit)(FILE *, const struct manifest *))
{
    char path[PKG_PATH_MAX], tmp[PKG_PATH_MAX];
    db_path(path, sizeof path, rel);
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
        fprintf(f, "%s=/%s\n", m->launchers[i].title, m->launchers[i].command);
}

static void emit_types(FILE *f, const struct manifest *m)
{
    for (int i = 0; i < m->ntypes; i++)
        fprintf(f, "%s %s\n", m->types[i].type, m->types[i].extensions);
}

static void emit_handlers(FILE *f, const struct manifest *m)
{
    for (int i = 0; i < m->nhandlers; i++)
        fprintf(f, "%s /%s\n", m->handlers[i].type, m->handlers[i].command);
}

int db_write_tables(void)
{
    char dir[PKG_PATH_MAX];
    db_dir(dir, sizeof dir, NULL);
    if (mkdir_all(dir) < 0)
        return -errno;
    int r = write_table("launcher", "# Launcher entries of installed packages, written by pkg", emit_launcher);
    if (r == 0)
        r = write_table("mime.types", "# type extensions, written by pkg", emit_types);
    if (r == 0)
        r = write_table("mime.apps", "# type program, written by pkg", emit_handlers);
    return r;
}

/* The ABI number of a system library from /lib/abi, or -1. The libraries
 * of the root image that no package provides have their numbers there
 * until the base system is packaged (docs/plan/packaging.md, P3). */
int system_abi(const char *soname)
{
    uint8_t *data;
    size_t len;
    char path[PKG_PATH_MAX];
    root_path(path, sizeof path, "lib/abi");
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
