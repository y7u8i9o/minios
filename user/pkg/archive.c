/* Packages are gzip compressed ustar archives (tar(1) and gzip(1) write
 * them). The whole archive is decompressed into memory; packages are
 * small and the checks need every member header before anything is
 * written. */
#include "pkg.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <minios/gzip.h>

#define BLOCK 512

struct ustar {
    char name[100], mode[8], uid[8], gid[8], size[12], mtime[12], chksum[8];
    char typeflag, linkname[100], magic[6], version[2], uname[32], gname[32];
    char devmajor[8], devminor[8], prefix[155], pad[12];
};

static uint64_t octal(const char *s, size_t n)
{
    uint64_t v = 0;
    for (size_t i = 0; i < n && s[i]; i++) {
        if (s[i] == ' ') continue;
        if (s[i] < '0' || s[i] > '7') break;
        v = v * 8 + (uint64_t)(s[i] - '0');
    }
    return v;
}

int archive_load(struct archive *a, const char *path, char *err, size_t errlen)
{
    uint8_t *raw;
    size_t rawlen;
    memset(a, 0, sizeof *a);
    if (read_file(path, &raw, &rawlen) < 0) {
        snprintf(err, errlen, "%s: %s", path, strerror(errno));
        return -1;
    }
    int r = gzip_decompress(raw, rawlen, &a->data, &a->len);
    free(raw);
    if (r < 0) {
        snprintf(err, errlen, "%s: not a gzip file", path);
        return -1;
    }
    if (a->len % BLOCK) {
        snprintf(err, errlen, "%s: archive size is not a multiple of %d", path, BLOCK);
        archive_free(a);
        return -1;
    }
    return 0;
}

void archive_rewind(struct archive *a) { a->at = 0; }

void archive_free(struct archive *a)
{
    free(a->data);
    memset(a, 0, sizeof *a);
}

/* The next member: 1 with m filled, 0 at the end, -1 for a damaged header
 * or a member kind the installer does not accept. */
int archive_next(struct archive *a, struct member *m, char *err, size_t errlen)
{
    for (;;) {
        if (a->at + BLOCK > a->len)
            return 0;
        const struct ustar *h = (const struct ustar *)(a->data + a->at);
        int zero = 1;
        for (size_t i = 0; i < BLOCK && zero; i++)
            if (a->data[a->at + i]) zero = 0;
        if (zero)
            return 0;
        if (memcmp(h->magic, "ustar", 5) != 0) {
            snprintf(err, errlen, "member at offset %zu is not a ustar header", a->at);
            return -1;
        }
        unsigned sum = 0;
        for (size_t i = 0; i < BLOCK; i++)
            sum += i >= 148 && i < 156 ? ' ' : a->data[a->at + i];
        if (sum != octal(h->chksum, sizeof h->chksum)) {
            snprintf(err, errlen, "member at offset %zu has a wrong checksum", a->at);
            return -1;
        }
        memset(m, 0, sizeof *m);
        size_t pn = strnlen(h->prefix, sizeof h->prefix), nn = strnlen(h->name, sizeof h->name);
        if (pn + 1 + nn >= sizeof m->path) {
            snprintf(err, errlen, "member name at offset %zu is too long", a->at);
            return -1;
        }
        if (pn) {
            memcpy(m->path, h->prefix, pn);
            m->path[pn] = '/';
            memcpy(m->path + pn + 1, h->name, nn);
        } else {
            memcpy(m->path, h->name, nn);
        }
        m->mode = (uint32_t)octal(h->mode, sizeof h->mode);
        m->mtime = (time_t)octal(h->mtime, sizeof h->mtime);
        m->size = (size_t)octal(h->size, sizeof h->size);
        size_t blocks = (m->size + BLOCK - 1) / BLOCK;
        if (a->at + BLOCK + blocks * BLOCK > a->len) {
            snprintf(err, errlen, "member %s is truncated", m->path);
            return -1;
        }
        m->data = a->data + a->at + BLOCK;
        a->at += BLOCK + blocks * BLOCK;
        /* Strip a leading ./ and a trailing slash, which host tars add. */
        char *p = m->path;
        while (p[0] == '.' && p[1] == '/') p += 2;
        while (*p == '/') p++;
        memmove(m->path, p, strlen(p) + 1);
        size_t l = strlen(m->path);
        while (l && m->path[l - 1] == '/') m->path[--l] = '\0';
        if (h->typeflag == '5' || (h->typeflag == '\0' && !m->size && l && h->name[strnlen(h->name, 100) - 1] == '/')) {
            m->dir = 1;
            m->size = 0;
        } else if (h->typeflag == '0' || h->typeflag == '\0' || h->typeflag == '7') {
            m->dir = 0;
        } else if (h->typeflag == 'x' || h->typeflag == 'g') {
            continue;               /* pax headers carry nothing the installer needs */
        } else {
            snprintf(err, errlen, "member %s is not a regular file or a directory", m->path);
            return -1;
        }
        if (!l)
            continue;
        return 1;
    }
}

/* ---- writing ---- */

static int grow(struct tar_writer *w, size_t more)
{
    if (w->len + more <= w->cap)
        return 0;
    size_t cap = w->cap ? w->cap * 2 : 65536;
    while (cap < w->len + more) cap *= 2;
    uint8_t *d = realloc(w->data, cap);
    if (!d)
        return -1;
    w->data = d;
    w->cap = cap;
    return 0;
}

static void put_octal(char *field, size_t n, uint64_t v)
{
    snprintf(field, n, "%0*llo", (int)n - 1, (unsigned long long)v);
}

int tarw_add(struct tar_writer *w, const char *path, int dir, uint32_t mode, time_t mtime, const uint8_t *data, size_t size)
{
    struct ustar h;
    memset(&h, 0, sizeof h);
    size_t l = strlen(path);
    if (l < sizeof h.name) {
        memcpy(h.name, path, l);
    } else {
        /* Split at a slash so that the prefix holds at most 155 bytes and
         * the name at most 100. */
        const char *cut = NULL;
        for (const char *s = strchr(path, '/'); s; s = strchr(s + 1, '/'))
            if ((size_t)(s - path) <= sizeof h.prefix && l - (size_t)(s - path) - 1 < sizeof h.name)
                cut = s;
        if (!cut)
            return -1;
        memcpy(h.prefix, path, (size_t)(cut - path));
        memcpy(h.name, cut + 1, l - (size_t)(cut - path) - 1);
    }
    put_octal(h.mode, sizeof h.mode, mode & 07777);
    put_octal(h.uid, sizeof h.uid, 0);
    put_octal(h.gid, sizeof h.gid, 0);
    put_octal(h.size, sizeof h.size, dir ? 0 : size);
    put_octal(h.mtime, sizeof h.mtime, (uint64_t)mtime);
    h.typeflag = dir ? '5' : '0';
    memcpy(h.magic, "ustar", 6);
    memcpy(h.version, "00", 2);
    memcpy(h.uname, "user", 4);
    memcpy(h.gname, "user", 4);
    memset(h.chksum, ' ', sizeof h.chksum);
    unsigned sum = 0;
    for (size_t i = 0; i < sizeof h; i++)
        sum += ((const uint8_t *)&h)[i];
    snprintf(h.chksum, sizeof h.chksum, "%06o", sum);
    h.chksum[7] = ' ';
    size_t blocks = dir ? 0 : (size + BLOCK - 1) / BLOCK;
    if (grow(w, BLOCK + blocks * BLOCK) < 0)
        return -1;
    memcpy(w->data + w->len, &h, BLOCK);
    w->len += BLOCK;
    if (!dir) {
        memcpy(w->data + w->len, data, size);
        memset(w->data + w->len + size, 0, blocks * BLOCK - size);
        w->len += blocks * BLOCK;
    }
    return 0;
}

int tarw_finish(struct tar_writer *w, const char *outpath)
{
    if (grow(w, 2 * BLOCK) < 0)
        return -1;
    memset(w->data + w->len, 0, 2 * BLOCK);
    w->len += 2 * BLOCK;
    uint8_t *gz;
    size_t gzlen;
    if (gzip_compress(w->data, w->len, &gz, &gzlen) < 0)
        return -1;
    int r = write_file(outpath, gz, gzlen);
    free(gz);
    free(w->data);
    memset(w, 0, sizeof *w);
    return r;
}
