#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <stdint.h>
#include "../thread/tcb.h"

typedef void (*emit_fn)(char c, void *arg);
int __vformat(emit_fn emit, void *arg, const char *fmt, va_list ap);

struct FILE {
    int fd;
    int mode;               /* _IOFBF, _IOLBF, _IONBF */
    int error, eof;
    char *buf;
    size_t size;
    size_t wpos;            /* bytes pending in buf for writing */
    size_t rpos, rlen;      /* read buffer window */
    char *tmppath;          /* tmpfile: removed when the stream closes */
    pid_t pid;              /* popen: the child that pclose waits for */
    struct __libc_lock lock; /* one operation at a time per stream (M35) */
    char inbuf[BUFSIZ];
};

#define LOCK(f) __libc_lock_lock(&(f)->lock)
#define UNLOCK(f) __libc_lock_unlock(&(f)->lock)

static struct FILE files[3];
FILE *stdin = &files[0], *stdout = &files[1], *stderr = &files[2];

/* Streams opened with fopen, flushed together by fflush(NULL) and exit. */
static struct FILE *open_streams[32];

void __stdio_init(void)
{
    for (int i = 0; i < 3; i++) {
        files[i].fd = i;
        files[i].buf = files[i].inbuf;
        files[i].size = BUFSIZ;
    }
    files[0].mode = _IOFBF;
    files[1].mode = _IOLBF;
    files[2].mode = _IONBF;
}

int fflush(FILE *f)
{
    if (!f) {
        fflush(stdout);
        fflush(stderr);
        for (size_t i = 0; i < sizeof open_streams / sizeof open_streams[0]; i++)
            if (open_streams[i])
                fflush(open_streams[i]);
        return 0;
    }
    LOCK(f);
    size_t off = 0;
    while (off < f->wpos) {
        ssize_t n = write(f->fd, f->buf + off, f->wpos - off);
        if (n < 0) {
            f->error = 1;
            f->wpos = 0;
            UNLOCK(f);
            return EOF;
        }
        off += (size_t)n;
    }
    f->wpos = 0;
    UNLOCK(f);
    return 0;
}

void __stdio_flush_all(void)
{
    fflush(NULL);
}

int fputc(int c, FILE *f)
{
    LOCK(f);
    if (f->wpos == f->size && fflush(f) == EOF) {
        UNLOCK(f);
        return EOF;
    }
    f->buf[f->wpos++] = (char)c;
    if (f->mode == _IONBF || (f->mode == _IOLBF && c == '\n')) {
        if (fflush(f) == EOF) {
            UNLOCK(f);
            return EOF;
        }
    }
    UNLOCK(f);
    return (unsigned char)c;
}

int putc(int c, FILE *f)
{
    return fputc(c, f);
}

int putchar(int c)
{
    return fputc(c, stdout);
}

int fputs(const char *s, FILE *f)
{
    LOCK(f);
    while (*s) {
        if (fputc(*s++, f) == EOF) {
            UNLOCK(f);
            return EOF;
        }
    }
    UNLOCK(f);
    return 0;
}

int puts(const char *s)
{
    LOCK(stdout);
    int r = fputs(s, stdout) == EOF || fputc('\n', stdout) == EOF ? EOF : 0;
    UNLOCK(stdout);
    return r;
}

size_t fwrite(const void *p, size_t size, size_t n, FILE *f)
{
    const char *s = p;
    size_t total = size * n;
    LOCK(f);
    for (size_t i = 0; i < total; i++) {
        if (fputc(s[i], f) == EOF) {
            UNLOCK(f);
            return i / size;
        }
    }
    UNLOCK(f);
    return n;
}

static void file_emit(char c, void *arg)
{
    fputc(c, arg);
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    LOCK(f);
    int n = __vformat(file_emit, f, fmt, ap);
    int error = f->error;
    UNLOCK(f);
    return error ? -1 : n;
}

int fprintf(FILE *f, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(f, fmt, ap);
    va_end(ap);
    return n;
}

int vprintf(const char *fmt, va_list ap)
{
    return vfprintf(stdout, fmt, ap);
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vfprintf(stdout, fmt, ap);
    va_end(ap);
    return n;
}

int dprintf(int fd, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof buf - 1)
        n = (int)sizeof buf - 1;
    return (int)write(fd, buf, (size_t)n);
}

static int fill(FILE *f)
{
    if (f == stdin)
        fflush(stdout);
    ssize_t n = read(f->fd, f->inbuf, sizeof f->inbuf);
    if (n <= 0) {
        if (n < 0)
            f->error = 1;
        else
            f->eof = 1;
        return EOF;
    }
    f->rpos = 0;
    f->rlen = (size_t)n;
    return 0;
}

ssize_t getdelim(char **line, size_t *capacity, int delimiter, FILE *f)
{
    if (!line || !capacity || !f) { errno = EINVAL; return -1; }
    LOCK(f);
    size_t used = 0;
    for (;;) {
        if (!*line || used + 1 >= *capacity) {
            size_t size = *line && *capacity ? *capacity * 2 : 128;
            if (size <= used + 1 || size > (size_t)PTRDIFF_MAX) {
                errno = EOVERFLOW; UNLOCK(f); return -1;
            }
            char *new_line = realloc(*line, size);
            if (!new_line) { UNLOCK(f); return -1; }
            *line = new_line; *capacity = size;
        }
        int c = fgetc(f);
        if (c == EOF) break;
        (*line)[used++] = (char)c;
        if ((unsigned char)c == (unsigned char)delimiter) break;
    }
    (*line)[used] = 0;
    ssize_t result = used ? (ssize_t)used : -1;
    UNLOCK(f);
    return result;
}

ssize_t getline(char **line, size_t *capacity, FILE *f)
{
    return getdelim(line, capacity, '\n', f);
}

int fgetc(FILE *f)
{
    LOCK(f);
    int c = EOF;
    if (f->rpos < f->rlen || fill(f) != EOF)
        c = (unsigned char)f->inbuf[f->rpos++];
    UNLOCK(f);
    return c;
}

int getc(FILE *f)
{
    return fgetc(f);
}

int getchar(void)
{
    return fgetc(stdin);
}

char *fgets(char *buf, int size, FILE *f)
{
    LOCK(f);
    int i = 0;
    while (i < size - 1) {
        int c = fgetc(f);
        if (c == EOF)
            break;
        buf[i++] = (char)c;
        if (c == '\n')
            break;
    }
    UNLOCK(f);
    if (i == 0)
        return NULL;
    buf[i] = '\0';
    return buf;
}

size_t fread(void *p, size_t size, size_t n, FILE *f)
{
    LOCK(f);
    char *d = p;
    size_t total = size * n, i;
    for (i = 0; i < total; i++) {
        int c = fgetc(f);
        if (c == EOF)
            break;
        d[i] = (char)c;
    }
    UNLOCK(f);
    return i / size;
}

int ferror(FILE *f) { return f->error; }
int feof(FILE *f) { return f->eof; }
void clearerr(FILE *f) { f->error = f->eof = 0; }
int fileno(FILE *f) { return f->fd; }

int setvbuf(FILE *f, char *buf, int mode, size_t size)
{
    fflush(f);
    f->mode = mode;
    if (buf && size) {
        f->buf = buf;
        f->size = size;
    }
    return 0;
}

void perror(const char *s)
{
    if (s && *s)
        fprintf(stderr, "%s: %s\n", s, strerror(errno));
    else
        fprintf(stderr, "%s\n", strerror(errno));
}

static int parse_mode(const char *mode, int *flags)
{
    int plus = strchr(mode, '+') != NULL;
    switch (mode[0]) {
    case 'r': *flags = plus ? O_RDWR : O_RDONLY; return 0;
    case 'w': *flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC; return 0;
    case 'a': *flags = (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND; return 0;
    }
    errno = EINVAL;
    return -1;
}

FILE *fdopen(int fd, const char *mode)
{
    int flags;
    if (parse_mode(mode, &flags) < 0)
        return NULL;
    FILE *f = calloc(1, sizeof *f);
    if (!f)
        return NULL;
    f->fd = fd;
    f->mode = _IOFBF;
    f->buf = f->inbuf;
    f->size = BUFSIZ;
    for (size_t i = 0; i < sizeof open_streams / sizeof open_streams[0]; i++) {
        if (!open_streams[i]) {
            open_streams[i] = f;
            break;
        }
    }
    return f;
}

FILE *fopen(const char *path, const char *mode)
{
    int flags;
    if (parse_mode(mode, &flags) < 0)
        return NULL;
    int fd = open(path, flags, 0644);
    if (fd < 0)
        return NULL;
    FILE *f = fdopen(fd, mode);
    if (!f)
        close(fd);
    return f;
}

int fclose(FILE *f)
{
    int r = fflush(f);
    if (f->fd >= 0 && close(f->fd) < 0)
        r = EOF;
    if (f->tmppath) {
        unlink(f->tmppath);
        free(f->tmppath);
        f->tmppath = NULL;
    }
    for (size_t i = 0; i < sizeof open_streams / sizeof open_streams[0]; i++)
        if (open_streams[i] == f)
            open_streams[i] = NULL;
    if (f != stdin && f != stdout && f != stderr)
        free(f);
    return r;
}

int fseek(FILE *f, long off, int whence)
{
    if (fflush(f) == EOF)
        return -1;
    if (whence == SEEK_CUR)
        off -= (long)(f->rlen - f->rpos);
    f->rpos = f->rlen = 0;
    f->eof = 0;
    return lseek(f->fd, off, whence) < 0 ? -1 : 0;
}

long ftell(FILE *f)
{
    off_t pos = lseek(f->fd, 0, SEEK_CUR);
    if (pos < 0)
        return -1;
    return (long)pos - (long)(f->rlen - f->rpos) + (long)f->wpos;
}

void rewind(FILE *f)
{
    fseek(f, 0, SEEK_SET);
    clearerr(f);
}

/* Pushes c back into the read buffer. After a read rpos is at least one,
 * so the byte fits in place; a push back onto an empty buffer shifts the
 * window instead. The pushed byte counts against the SEEK_CUR offset
 * like any unread byte. */
int ungetc(int c, FILE *f)
{
    if (c == EOF)
        return EOF;
    LOCK(f);
    if (f->rpos > 0) {
        f->inbuf[--f->rpos] = (char)c;
    } else if (f->rlen < sizeof f->inbuf) {
        memmove(f->inbuf + 1, f->inbuf, f->rlen);
        f->inbuf[0] = (char)c;
        f->rlen++;
    } else {
        UNLOCK(f);
        return EOF;
    }
    f->eof = 0;
    UNLOCK(f);
    return (unsigned char)c;
}

/* Replaces the file behind an existing stream; the standard streams
 * keep their identity so that stdin can be turned into a file. On
 * failure the stream is closed, as the standard requires. */
FILE *freopen(const char *path, const char *mode, FILE *f)
{
    int flags;
    if (parse_mode(mode, &flags) < 0) {
        fclose(f);
        return NULL;
    }
    fflush(f);
    close(f->fd);
    f->fd = -1;
    f->rpos = f->rlen = f->wpos = 0;
    f->error = f->eof = 0;
    int fd = open(path, flags, 0644);
    if (fd < 0) {
        fclose(f);
        return NULL;
    }
    f->fd = fd;
    return f;
}

/* Names live under P_tmpdir and carry the pid, so concurrent processes
 * never collide; the counter separates calls within one process. */
char *tmpnam(char *buf)
{
    static char names[L_tmpnam];
    static unsigned counter;
    char *out = buf ? buf : names;
    snprintf(out, L_tmpnam, P_tmpdir "/t%d.%u", (int)getpid(),
             __atomic_fetch_add(&counter, 1, __ATOMIC_RELAXED));
    return out;
}

/* A read/write stream on a fresh file that fclose removes. */
FILE *tmpfile(void)
{
    char name[L_tmpnam];
    int fd = -1;
    for (int attempt = 0; attempt < 16 && fd < 0; attempt++) {
        tmpnam(name);
        fd = open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd < 0 && errno != EEXIST)
            return NULL;
    }
    if (fd < 0)
        return NULL;
    FILE *f = fdopen(fd, "w+");
    if (!f) {
        close(fd);
        unlink(name);
        return NULL;
    }
    f->tmppath = strdup(name);
    return f;
}

/* The lock is not recursive, so a caller holding it through flockfile
 * must read with getc_unlocked. */
void flockfile(FILE *f) { LOCK(f); }
void funlockfile(FILE *f) { UNLOCK(f); }

int getc_unlocked(FILE *f)
{
    if (f->rpos < f->rlen || fill(f) != EOF)
        return (unsigned char)f->inbuf[f->rpos++];
    return EOF;
}

/* Offsets fit in long on x86-64, so the off_t forms share one path. */
int fseeko(FILE *f, off_t off, int whence)
{
    return fseek(f, (long)off, whence);
}

off_t ftello(FILE *f)
{
    return (off_t)ftell(f);
}

/* Runs command through /bin/sh -c with one end of a pipe as its stdin
 * ("w") or stdout ("r"); the other end becomes the stream. pclose
 * closes the stream and returns the child's wait status. */
FILE *popen(const char *command, const char *mode)
{
    int reading = mode[0] == 'r';
    if (mode[0] != 'r' && mode[0] != 'w') {
        errno = EINVAL;
        return NULL;
    }
    int fds[2];
    if (pipe(fds) < 0)
        return NULL;
    int parent_end = reading ? fds[0] : fds[1];
    int child_end = reading ? fds[1] : fds[0];
    FILE *f = fdopen(parent_end, mode);
    if (!f) {
        close(fds[0]);
        close(fds[1]);
        return NULL;
    }
    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) {
        fclose(f);
        close(child_end);
        return NULL;
    }
    if (pid == 0) {
        dup2(child_end, reading ? 1 : 0);
        close(child_end);
        close(parent_end);
        char *const argv[] = { "sh", "-c", (char *)command, NULL };
        execv("/bin/sh", argv);
        _exit(127);
    }
    close(child_end);
    f->pid = pid;
    return f;
}

int pclose(FILE *f)
{
    pid_t pid = f->pid;
    if (pid <= 0) {
        errno = EINVAL;
        return -1;
    }
    fclose(f);
    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    return status;
}

int vasprintf(char **out, const char *fmt, va_list ap)
{
    va_list copy;
    va_copy(copy, ap);
    int n = vsnprintf(NULL, 0, fmt, copy);
    va_end(copy);
    if (n < 0)
        return -1;
    char *buf = malloc((size_t)n + 1);
    if (buf == NULL)
        return -1;
    vsnprintf(buf, (size_t)n + 1, fmt, ap);
    *out = buf;
    return n;
}

int asprintf(char **out, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vasprintf(out, fmt, ap);
    va_end(ap);
    return n;
}
