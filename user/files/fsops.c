/* File operations of the Files program: path helpers, recursive copy,
 * remove and move, sizes. Errors are -errno. */
#include "files.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>

void fs_join(char *out, size_t size, const char *dir, const char *name)
{
    if (strcmp(dir, "/") == 0)
        snprintf(out, size, "/%s", name);
    else
        snprintf(out, size, "%s/%s", dir, name);
}

void fs_normalize(char *path)
{
    char *parts[64];
    int n = 0;
    char copy[512];
    strlcpy(copy, path, sizeof copy);
    for (char *p = strtok(copy, "/"); p; p = strtok(NULL, "/")) {
        if (strcmp(p, ".") == 0 || !p[0])
            continue;
        if (strcmp(p, "..") == 0) {
            if (n > 0)
                n--;
            continue;
        }
        if (n < 64)
            parts[n++] = p;
    }
    size_t len = 0;
    path[0] = '\0';
    for (int i = 0; i < n; i++) {
        len += (size_t)snprintf(path + len, 512 - len, "/%s", parts[i]);
        if (len >= 511)
            break;
    }
    if (n == 0)
        strcpy(path, "/");
}

const char *fs_basename(const char *path)
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
int fs_copy(const char *src, const char *dst)
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
        fs_join(from, sizeof from, src, e->d_name);
        fs_join(to, sizeof to, dst, e->d_name);
        err = fs_copy(from, to);
    }
    closedir(d);
    return err;
}

/* A symbolic link is removed itself; the tree it leads to is kept. */
int fs_remove(const char *path)
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
        fs_join(child, sizeof child, path, e->d_name);
        err = fs_remove(child);
    }
    closedir(d);
    if (err)
        return err;
    return rmdir(path) < 0 ? -errno : 0;
}

int fs_move(const char *src, const char *dst)
{
    if (rename(src, dst) == 0)
        return 0;
    if (errno != EXDEV)
        return -errno;
    int err = fs_copy(src, dst);
    if (err)
        return err;
    return fs_remove(src);
}

long fs_tree_size(const char *path, int *files, int *dirs)
{
    struct stat st;
    if (lstat(path, &st) < 0)
        return 0;
    if (!S_ISDIR(st.st_mode)) {
        (*files)++;
        return (long)st.st_size;
    }
    (*dirs)++;
    long total = 0;
    DIR *d = opendir(path);
    if (!d)
        return 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char child[512];
        fs_join(child, sizeof child, path, e->d_name);
        total += fs_tree_size(child, files, dirs);
    }
    closedir(d);
    return total;
}

const char *fs_human_size(long size, char *buf, size_t size_buf)
{
    if (size < 1024) {
        snprintf(buf, size_buf, "%ld bytes", size);
    } else if (size < 1024 * 1024) {
        long tenths = size * 10 / 1024;
        snprintf(buf, size_buf, "%ld.%ld KB", tenths / 10, tenths % 10);
    } else if (size < 1024L * 1024 * 1024) {
        long tenths = size * 10 / (1024 * 1024);
        snprintf(buf, size_buf, "%ld.%ld MB", tenths / 10, tenths % 10);
    } else {
        long tenths = size * 10 / (1024L * 1024 * 1024);
        snprintf(buf, size_buf, "%ld.%ld GB", tenths / 10, tenths % 10);
    }
    return buf;
}
