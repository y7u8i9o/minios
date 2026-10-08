/* File operations shared by Files, the file chooser and the desktop:
 * recursive copy, remove and move (formerly in Files), the text/uri-list
 * of dragged files, and the drop of files into a folder. */
#include <gui/fileops.h>
#include <gui/client.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libintl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "intl.h"

static void join(char *out, size_t size, const char *dir, const char *name)
{
    if (strcmp(dir, "/") == 0)
        snprintf(out, size, "/%s", name);
    else
        snprintf(out, size, "%s/%s", dir, name);
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    if (!slash || !slash[1])
        return path[0] == '/' && !path[1] ? "/" : path;
    return slash + 1;
}

static int copy_file(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY);
    if (in < 0)
        return -errno;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        int err = -errno;
        close(in);
        return err;
    }
    static char buf[65536];
    int err = 0;
    for (;;) {
        ssize_t n = read(in, buf, sizeof buf);
        if (n < 0) {
            err = -errno;
            break;
        }
        if (n == 0)
            break;
        for (ssize_t off = 0; off < n;) {
            ssize_t k = write(out, buf + off, (size_t)(n - off));
            if (k <= 0) {
                err = k < 0 ? -errno : -EIO;
                break;
            }
            off += k;
        }
        if (err)
            break;
    }
    close(in);
    close(out);
    return err;
}

/* A symbolic link is copied as a link with the same target, so a link to
 * a directory is not entered and a link to a parent cannot recurse. */
int fileops_copy(const char *src, const char *dst)
{
    struct stat st;
    if (lstat(src, &st) < 0)
        return -errno;
    if (S_ISLNK(st.st_mode)) {
        char target[512];
        ssize_t n = readlink(src, target, sizeof target - 1);
        if (n < 0)
            return -errno;
        target[n] = '\0';
        return symlink(target, dst) < 0 ? -errno : 0;
    }
    if (!S_ISDIR(st.st_mode))
        return copy_file(src, dst);
    if (mkdir(dst, 0755) < 0 && errno != EEXIST)
        return -errno;
    DIR *d = opendir(src);
    if (!d)
        return -errno;
    int err = 0;
    struct dirent *e;
    while (!err && (e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char from[512], to[512];
        join(from, sizeof from, src, e->d_name);
        join(to, sizeof to, dst, e->d_name);
        err = fileops_copy(from, to);
    }
    closedir(d);
    return err;
}

/* A symbolic link is removed itself; the tree it leads to is retained. */
int fileops_remove(const char *path)
{
    struct stat st;
    if (lstat(path, &st) < 0)
        return -errno;
    if (!S_ISDIR(st.st_mode))
        return unlink(path) < 0 ? -errno : 0;
    DIR *d = opendir(path);
    if (!d)
        return -errno;
    int err = 0;
    struct dirent *e;
    while (!err && (e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char child[512];
        join(child, sizeof child, path, e->d_name);
        err = fileops_remove(child);
    }
    closedir(d);
    if (err)
        return err;
    return rmdir(path) < 0 ? -errno : 0;
}

int fileops_move(const char *src, const char *dst)
{
    if (rename(src, dst) == 0)
        return 0;
    if (errno != EXDEV)
        return -errno;
    int err = fileops_copy(src, dst);
    if (err)
        return err;
    return fileops_remove(src);
}

/* ---- text/uri-list ---- */

static int unreserved(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' ||
           c == '_' || c == '~' || c == '/';
}

char *fileops_uri_list(const char *const *paths, int n)
{
    size_t size = 1;
    for (int i = 0; i < n; i++)
        size += strlen("file://") + 3 * strlen(paths[i]) + 2;
    char *out = malloc(size), *o = out;
    if (!out)
        return NULL;
    for (int i = 0; i < n; i++) {
        o += sprintf(o, "file://");
        for (const unsigned char *p = (const unsigned char *)paths[i]; *p; p++)
            o += unreserved(*p) ? sprintf(o, "%c", *p) : sprintf(o, "%%%02X", *p);
        *o++ = '\r';
        *o++ = '\n';
    }
    *o = '\0';
    return out;
}

static int hex(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static int add_path(char ***paths, int n, const char *start, size_t len)
{
    char **grown = realloc(*paths, (size_t)(n + 1) * sizeof **paths);
    char *p = malloc(len + 1);
    if (!grown || !p) {
        free(p);
        if (grown)
            *paths = grown;
        return n;
    }
    memcpy(p, start, len);
    p[len] = '\0';
    *paths = grown;
    grown[n] = p;
    return n + 1;
}

int fileops_parse_uri_list(const char *text, size_t len, char ***paths)
{
    *paths = NULL;
    int n = 0;
    const char *end = text + len;
    for (const char *line = text; line < end;) {
        const char *eol = memchr(line, '\n', (size_t)(end - line));
        if (!eol)
            eol = end;
        const char *stop = eol > line && eol[-1] == '\r' ? eol - 1 : eol;
        const char *p = line;
        line = eol + 1;
        /* file:///path and file://localhost/path name local files. */
        if (stop - p < 8 || strncmp(p, "file://", 7) != 0)
            continue;
        p += 7;
        if (stop - p >= 9 && strncmp(p, "localhost", 9) == 0)
            p += 9;
        if (p >= stop || *p != '/')
            continue;
        char buf[512];
        size_t k = 0;
        for (; p < stop && k < sizeof buf - 1; p++) {
            int h1, h2;
            if (*p == '%' && stop - p >= 3 && (h1 = hex(p[1])) >= 0 && (h2 = hex(p[2])) >= 0) {
                buf[k++] = (char)(h1 << 4 | h2);
                p += 2;
            } else {
                buf[k++] = *p;
            }
        }
        n = add_path(paths, n, buf, k);
    }
    return n;
}

void fileops_free_paths(char **paths, int n)
{
    for (int i = 0; i < n; i++)
        free(paths[i]);
    free(paths);
}

int fileops_parse_drop(const char *mime, const char *text, size_t len, char ***paths)
{
    if (mime && strcmp(mime, "text/uri-list") == 0)
        return fileops_parse_uri_list(text, len, paths);
    *paths = NULL;
    int n = 0;
    const char *end = text + len;
    for (const char *line = text; line < end;) {
        const char *eol = memchr(line, '\n', (size_t)(end - line));
        if (!eol)
            eol = end;
        size_t l = (size_t)(eol - line);
        if (l && line[l - 1] == '\r')
            l--;
        char buf[512];
        struct stat st;
        if (l && l < sizeof buf && line[0] == '/') {
            memcpy(buf, line, l);
            buf[l] = '\0';
            if (lstat(buf, &st) == 0)
                n = add_path(paths, n, buf, l);
        }
        line = eol + 1;
    }
    return n;
}

/* ---- drops ---- */

int fileops_inside(const char *path, const char *dir)
{
    size_t n = strlen(dir);
    if (n > 1 && dir[n - 1] == '/')
        n--;
    if (strcmp(dir, "/") == 0)
        return 1;
    return strncmp(path, dir, n) == 0 && (path[n] == '\0' || path[n] == '/');
}

int fileops_same_volume(const char *src, const char *dir)
{
    struct stat a, b;
    return lstat(src, &a) == 0 && stat(dir, &b) == 0 && a.st_dev == b.st_dev;
}

static void parent_of(const char *path, char *out, size_t size)
{
    strlcpy(out, path, size);
    char *slash = strrchr(out, '/');
    if (slash)
        *(slash == out ? slash + 1 : slash) = '\0';
}

int fileops_drop_action(const char *uris, size_t len, const char *dest)
{
    if (!uris)
        return GUI_DND_COPY;
    char **paths;
    int n = fileops_parse_uri_list(uris, len, &paths), same = n > 0, here = n > 0, inside = 0;
    for (int i = 0; i < n; i++) {
        char dir[512];
        parent_of(paths[i], dir, sizeof dir);
        if (fileops_inside(dest, paths[i]))
            inside = 1;
        if (strcmp(dir, dest) != 0)
            here = 0;
        if (same && !fileops_same_volume(paths[i], dest))
            same = 0;
    }
    fileops_free_paths(paths, n);
    if (inside || here)
        return 0;
    return same ? GUI_DND_MOVE : GUI_DND_COPY;
}

int fileops_drop(char *const *paths, int n, const char *dir, int action, const char **failed)
{
    int done = 0;
    *failed = NULL;
    for (int i = 0; i < n; i++) {
        const char *src = paths[i];
        char from_dir[512], to[512];
        parent_of(src, from_dir, sizeof from_dir);
        join(to, sizeof to, dir, base_name(src));
        int err = 0;
        struct stat st;
        if (strcmp(from_dir, dir) == 0) {
            if (action == GUI_DND_MOVE)
                continue;
            char name[300];
            snprintf(name, sizeof name, _("Copy of %s"), base_name(src));
            join(to, sizeof to, dir, name);
        }
        if (fileops_inside(dir, src))
            err = -EINVAL;
        else if (lstat(to, &st) == 0)
            err = -EEXIST;
        else
            err = action == GUI_DND_MOVE ? fileops_move(src, to) : fileops_copy(src, to);
        if (err) {
            *failed = src;
            return err;
        }
        done++;
    }
    return done;
}
