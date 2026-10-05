/* Messages, memory, files and the iteration over objects (binutils.h). */
#include "binutils.h"
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

const char *bu_program = "binutils";

static void message(const char *file, const char *fmt, va_list ap)
{
    fflush(stdout);
    if (file)
        fprintf(stderr, "%s: %s: ", bu_program, file);
    else
        fprintf(stderr, "%s: ", bu_program);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
}

void bu_error(const char *file, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    message(file, fmt, ap);
    va_end(ap);
}

void bu_fatal(const char *file, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    message(file, fmt, ap);
    va_end(ap);
    exit(1);
}

void *bu_alloc(size_t size)
{
    void *p = malloc(size ? size : 1);
    if (!p)
        bu_fatal(NULL, "out of memory");
    return p;
}

void *bu_realloc(void *p, size_t size)
{
    p = realloc(p, size ? size : 1);
    if (!p)
        bu_fatal(NULL, "out of memory");
    return p;
}

char *bu_strndup(const char *s, size_t n)
{
    char *copy = bu_alloc(n + 1);
    memcpy(copy, s, n);
    copy[n] = '\0';
    return copy;
}

char *bu_strdup(const char *s)
{
    return bu_strndup(s, strlen(s));
}

int bu_read_file(const char *path, uint8_t **data, size_t *size)
{
    *data = NULL;
    *size = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -errno;
    struct stat st;
    if (fstat(fd, &st) < 0) {
        int err = errno;
        close(fd);
        return -err;
    }
    if (S_ISDIR(st.st_mode)) {
        close(fd);
        return -EISDIR;
    }
    size_t n = (size_t)st.st_size, done = 0;
    uint8_t *buf = bu_alloc(n);
    while (done < n) {
        ssize_t r = read(fd, buf + done, n - done);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {
            int err = r < 0 ? errno : EIO;
            free(buf);
            close(fd);
            return -err;
        }
        done += (size_t)r;
    }
    close(fd);
    *data = buf;
    *size = n;
    return 0;
}

int bu_write_file(const char *path, const void *data, size_t size, unsigned mode)
{
    size_t len = strlen(path) + 16;
    char *tmp = bu_alloc(len);
    snprintf(tmp, len, "%s.tmp%d", path, (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
    int err = 0;
    if (fd < 0) {
        err = errno;
    } else {
        const uint8_t *p = data;
        while (size > 0) {
            ssize_t w = write(fd, p, size);
            if (w < 0 && errno == EINTR)
                continue;
            if (w <= 0) {
                err = w < 0 ? errno : EIO;
                break;
            }
            p += w;
            size -= (size_t)w;
        }
        if (close(fd) < 0 && !err)
            err = errno;
        if (!err && fchmodat(AT_FDCWD, tmp, mode, 0) < 0)
            err = errno;
        if (!err && rename(tmp, path) < 0)
            err = errno;
        if (err)
            unlink(tmp);
    }
    free(tmp);
    return -err;
}

int bu_for_each_object(const char *path, bu_archive_fn archive_fn, bu_object_fn fn, bu_other_fn other_fn,
                       void *arg)
{
    uint8_t *data;
    size_t size;
    int r = bu_read_file(path, &data, &size);
    if (r == -ENOENT) {
        bu_error(NULL, "'%s': No such file", path);
        return 1;
    }
    if (r == -EISDIR) {
        bu_error(NULL, "Warning: '%s' is a directory", path);
        return 1;
    }
    if (r < 0) {
        bu_error(path, "%s", strerror(-r));
        return 1;
    }
    int status = 0;
    struct elffile f;
    if (archive_is(data, size)) {
        struct archive a;
        char err[200];
        if (archive_parse(&a, data, size, err, sizeof err) < 0) {
            bu_error(path, "%s", err);
            free(data);
            return 1;
        }
        if (archive_fn && archive_fn(path, &a, arg))
            status = 1;
        for (struct ar_member *m = a.members; m; m = m->next) {
            if (elffile_open(&f, m->data, m->size) < 0) {
                if (other_fn)
                    other_fn(path, m->name, arg);
                bu_error(path, "%s: file format not recognized", m->name);
                status = 1;
                continue;
            }
            if (fn(path, m->name, &f, arg))
                status = 1;
        }
        archive_free(&a);
    } else if (elffile_open(&f, data, size) < 0) {
        bu_error(path, "file format not recognized");
        status = 1;
    } else if (fn(path, NULL, &f, arg)) {
        status = 1;
    }
    free(data);
    return status;
}
