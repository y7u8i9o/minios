/* gzip: the command over the codec in libc (minios/gzip.h). */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <minios/gzip.h>

static uint8_t *read_all(FILE *f, size_t *length)
{
    size_t len = 0, cap = 4096;
    uint8_t *p = malloc(cap);
    if (!p)
        return NULL;
    for (;;) {
        if (len == cap) {
            cap *= 2;
            uint8_t *q = realloc(p, cap);
            if (!q) { free(p); return NULL; }
            p = q;
        }
        size_t n = fread(p + len, 1, cap - len, f);
        len += n;
        if (!n)
            break;
    }
    if (ferror(f)) { free(p); return NULL; }
    *length = len;
    return p;
}

static int write_all(FILE *f, const uint8_t *p, size_t n)
{
    return fwrite(p, 1, n, f) == n ? 0 : -1;
}

static int convert(const char *path, int decompress, int stdout_mode, int retain, int test)
{
    FILE *in = path ? fopen(path, "r") : stdin;
    if (!in) {
        fprintf(stderr, "gzip: %s: %s\n", path, strerror(errno));
        return 1;
    }
    size_t srclen, dstlen;
    uint8_t *src = read_all(in, &srclen), *dst = NULL;
    if (path)
        fclose(in);
    if (!src) {
        fprintf(stderr, "gzip: %s: read error\n", path ? path : "standard input");
        return 1;
    }
    int r = decompress ? gzip_decompress(src, srclen, &dst, &dstlen) :
                         gzip_compress(src, srclen, &dst, &dstlen);
    free(src);
    if (r < 0) {
        fprintf(stderr, "gzip: %s: invalid data or out of memory\n", path ? path : "standard input");
        return 1;
    }
    if (test) {
        free(dst);
        return 0;
    }
    if (!path || stdout_mode) {
        r = write_all(stdout, dst, dstlen);
    } else {
        char output[1024];
        size_t n = strlen(path);
        if (decompress) {
            if (n <= 3 || strcmp(path + n - 3, ".gz") != 0) {
                fprintf(stderr, "gzip: %s: unknown suffix\n", path);
                free(dst);
                return 1;
            }
            snprintf(output, sizeof output, "%.*s", (int)(n - 3), path);
        } else {
            snprintf(output, sizeof output, "%s.gz", path);
        }
        int fd = open(output, O_WRONLY | O_CREAT | O_EXCL, 0644);
        if (fd < 0) {
            fprintf(stderr, "gzip: %s: %s\n", output, strerror(errno));
            free(dst);
            return 1;
        }
        FILE *out = fdopen(fd, "w");
        r = out ? write_all(out, dst, dstlen) : -1;
        if (out)
            fclose(out);
        else
            close(fd);
        if (r < 0) {
            fprintf(stderr, "gzip: %s: write error\n", output);
            unlink(output);
        } else if (!retain) {
            unlink(path);
        }
    }
    free(dst);
    return r < 0 ? 1 : 0;
}

int main(int argc, char **argv)
{
    int decompress = 0, stdout_mode = 0, retain = 0, test = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (strcmp(argv[i], "--") == 0) { i++; break; }
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'c') stdout_mode = 1;
            else if (*p == 'd') decompress = 1;
            else if (*p == 'k') retain = 1;
            else if (*p == 't') { test = 1; decompress = 1; }
            else if (*p == 'f') ;   /* force: output files are always replaced */
            else {
                fprintf(stderr, "usage: gzip [-cdfkt] [file...]\n");
                return 2;
            }
        }
    }
    if (i == argc)
        return convert(NULL, decompress, 1, 1, test);
    int status = 0;
    for (; i < argc; i++)
        status |= convert(argv[i], decompress, stdout_mode, retain, test);
    return status;
}
