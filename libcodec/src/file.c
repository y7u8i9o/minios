/* Whole files in and out of memory. */
#include <codec/codec.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

int codec_read_file(const char *path, uint8_t **data, size_t *len)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -errno;
    size_t cap = 4096, n = 0;
    uint8_t *buf = malloc(cap);
    while (buf) {
        n += fread(buf + n, 1, cap - n, f);
        if (n < cap)
            break;
        uint8_t *grown = realloc(buf, cap * 2);
        if (!grown) {
            free(buf);
            buf = NULL;
            break;
        }
        buf = grown;
        cap *= 2;
    }
    int err = ferror(f) ? (errno ? errno : EIO) : 0;
    fclose(f);
    if (!buf)
        return -ENOMEM;
    if (err) {
        free(buf);
        return -err;
    }
    *data = buf;
    *len = n;
    return 0;
}

int codec_write_file(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return -errno;
    size_t done = fwrite(data, 1, len, f);
    int e = done == len ? 0 : errno ? errno : EIO;
    if (fclose(f) != 0 && !e)
        e = errno ? errno : EIO;
    return -e;
}
