#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>

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
    char inbuf[BUFSIZ];
};

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
    size_t off = 0;
    while (off < f->wpos) {
        ssize_t n = write(f->fd, f->buf + off, f->wpos - off);
        if (n < 0) {
            f->error = 1;
            f->wpos = 0;
            return EOF;
        }
        off += (size_t)n;
    }
    f->wpos = 0;
    return 0;
}

void __stdio_flush_all(void)
{
    fflush(NULL);
}

int fputc(int c, FILE *f)
{
    if (f->wpos == f->size && fflush(f) == EOF)
        return EOF;
    f->buf[f->wpos++] = (char)c;
    if (f->mode == _IONBF || (f->mode == _IOLBF && c == '\n')) {
        if (fflush(f) == EOF)
            return EOF;
    }
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
    while (*s) {
        if (fputc(*s++, f) == EOF)
            return EOF;
    }
    return 0;
}

int puts(const char *s)
{
    if (fputs(s, stdout) == EOF || fputc('\n', stdout) == EOF)
        return EOF;
    return 0;
}

size_t fwrite(const void *p, size_t size, size_t n, FILE *f)
{
    const char *s = p;
    size_t total = size * n;
    for (size_t i = 0; i < total; i++) {
        if (fputc(s[i], f) == EOF)
            return i / size;
    }
    return n;
}

static void file_emit(char c, void *arg)
{
    fputc(c, arg);
}

int vfprintf(FILE *f, const char *fmt, va_list ap)
{
    int n = __vformat(file_emit, f, fmt, ap);
    if (f->error)
        return -1;
    return n;
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

int fgetc(FILE *f)
{
    if (f->rpos >= f->rlen && fill(f) == EOF)
        return EOF;
    return (unsigned char)f->inbuf[f->rpos++];
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
    int i = 0;
    while (i < size - 1) {
        int c = fgetc(f);
        if (c == EOF)
            break;
        buf[i++] = (char)c;
        if (c == '\n')
            break;
    }
    if (i == 0)
        return NULL;
    buf[i] = '\0';
    return buf;
}

size_t fread(void *p, size_t size, size_t n, FILE *f)
{
    char *d = p;
    size_t total = size * n, i;
    for (i = 0; i < total; i++) {
        int c = fgetc(f);
        if (c == EOF)
            break;
        d[i] = (char)c;
    }
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
    if (close(f->fd) < 0)
        r = EOF;
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
