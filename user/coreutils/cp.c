/* cp: copy files.
 *
 *   cp [-R] [-H | -L | -P] source target
 *   cp [-R] [-H | -L | -P] source... directory
 *
 * -R copies directories recursively. A symbolic link among the sources is
 * copied as a link (a new link holding the same target) with -P, and
 * followed so that the file it leads to is copied with -L; -H follows the
 * links named as operands only. Without an option cp follows links, and
 * with -R alone it copies them as links. A target that is an existing
 * directory receives the sources under their last path component. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>

#define PATH_SIZE 1024

static int recursive;
static char follow;                 /* 'H', 'L' or 'P' */

static int fail(const char *path)
{
    fprintf(stderr, "cp: %s: %s\n", path, strerror(errno));
    return 1;
}

static int copy_data(const char *src, const char *dst, unsigned mode)
{
    int in = open(src, O_RDONLY);
    if (in < 0)
        return fail(src);
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode & 0777);
    if (out < 0) {
        int status = fail(dst);
        close(in);
        return status;
    }
    char buf[4096];
    ssize_t n;
    int status = 0;
    while ((n = read(in, buf, sizeof buf)) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t w = write(out, buf + off, (size_t)(n - off));
            if (w < 0) {
                status = fail(dst);
                n = 0;
                break;
            }
            off += w;
        }
    }
    if (n < 0)
        status = fail(src);
    close(in);
    close(out);
    return status;
}

/* Create dst as a new symbolic link with the target of the link src. An
 * existing file or link at dst is replaced. */
static int copy_link(const char *src, const char *dst)
{
    char target[PATH_SIZE];
    ssize_t n = readlink(src, target, sizeof target - 1);
    if (n < 0)
        return fail(src);
    target[n] = '\0';
    struct stat st;
    if (lstat(dst, &st) == 0 && !S_ISDIR(st.st_mode) && unlink(dst) < 0)
        return fail(dst);
    if (symlink(target, dst) < 0)
        return fail(dst);
    return 0;
}

static int copy(const char *src, const char *dst, int depth)
{
    struct stat st;
    int follows = follow == 'L' || (follow == 'H' && depth == 0);
    if ((follows ? stat(src, &st) : lstat(src, &st)) < 0)
        return fail(src);
    if (S_ISLNK(st.st_mode))
        return copy_link(src, dst);
    if (!S_ISDIR(st.st_mode))
        return copy_data(src, dst, st.st_mode);
    if (!recursive) {
        fprintf(stderr, "cp: %s: is a directory (not copied without -R)\n", src);
        return 1;
    }
    if (mkdir(dst, st.st_mode & 0777) < 0) {
        struct stat dst_st;
        if (errno != EEXIST || stat(dst, &dst_st) < 0 || !S_ISDIR(dst_st.st_mode))
            return fail(dst);
    }
    DIR *d = opendir(src);
    if (!d)
        return fail(src);
    int status = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        char from[PATH_SIZE], to[PATH_SIZE];
        if ((size_t)snprintf(from, sizeof from, "%s/%s", src, e->d_name) >= sizeof from ||
            (size_t)snprintf(to, sizeof to, "%s/%s", dst, e->d_name) >= sizeof to) {
            fprintf(stderr, "cp: %s/%s: %s\n", src, e->d_name, strerror(ENAMETOOLONG));
            status = 1;
            continue;
        }
        status |= copy(from, to, depth + 1);
    }
    closedir(d);
    return status;
}

static const char *last_component(const char *path, char *buf, size_t size)
{
    size_t n = strlen(path);
    while (n > 1 && path[n - 1] == '/')
        n--;
    size_t end = n;
    while (n > 0 && path[n - 1] != '/')
        n--;
    size_t len = end - n < size ? end - n : size - 1;
    memcpy(buf, path + n, len);
    buf[len] = '\0';
    return buf;
}

static int usage(void)
{
    fprintf(stderr, "usage: cp [-R] [-H | -L | -P] source... target\n");
    return 1;
}

int main(int argc, char **argv)
{
    int first = 1;
    for (; first < argc && argv[first][0] == '-' && argv[first][1]; first++) {
        if (strcmp(argv[first], "--") == 0) {
            first++;
            break;
        }
        for (const char *p = argv[first] + 1; *p; p++) {
            if (*p == 'R' || *p == 'r')
                recursive = 1;
            else if (*p == 'H' || *p == 'L' || *p == 'P')
                follow = *p;
            else
                return usage();
        }
    }
    if (!follow)
        follow = recursive ? 'P' : 'L';
    int count = argc - first - 1;
    if (count < 1)
        return usage();
    const char *target = argv[argc - 1];
    struct stat st;
    int into_dir = stat(target, &st) == 0 && S_ISDIR(st.st_mode);
    if (count > 1 && !into_dir) {
        errno = ENOTDIR;
        return fail(target);
    }
    int status = 0;
    for (int i = first; i < argc - 1; i++) {
        if (!into_dir) {
            status |= copy(argv[i], target, 0);
            continue;
        }
        char name[256], dst[PATH_SIZE];
        last_component(argv[i], name, sizeof name);
        if ((size_t)snprintf(dst, sizeof dst, "%s/%s", target, name) >= sizeof dst) {
            errno = ENAMETOOLONG;
            status |= fail(argv[i]);
            continue;
        }
        status |= copy(argv[i], dst, 0);
    }
    return status;
}
