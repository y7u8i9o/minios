/* gzip: RFC 1952 framing, RFC 1951 inflate, and a fixed-Huffman LZ77
 * compressor. The decoder accepts stored, fixed, and dynamic blocks. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>

#define MAXBITS 15
#define MAXLCODES 286
#define MAXDCODES 30
#define FIXLCODES 288

static const short lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const short lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const short dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const short dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static uint32_t crc32_data(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xffffffffu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (crc & 1 ? 0xedb88320u : 0);
    }
    return ~crc;
}

struct bits {
    const uint8_t *in;
    size_t len, pos;
    uint32_t buf;
    int nbits;
    uint8_t *out;
    size_t cap, outlen;
};

static int need(struct bits *b, int n)
{
    while (b->nbits < n) {
        if (b->pos >= b->len)
            return -1;
        b->buf |= (uint32_t)b->in[b->pos++] << b->nbits;
        b->nbits += 8;
    }
    return 0;
}

static int getbits(struct bits *b, int n)
{
    if (!n)
        return 0;
    if (need(b, n) < 0)
        return -1;
    int value = (int)(b->buf & ((1u << n) - 1));
    b->buf >>= n;
    b->nbits -= n;
    return value;
}

struct huff {
    short count[MAXBITS + 1];
    short symbol[FIXLCODES];
};

static int build(struct huff *h, const short *length, int n)
{
    short offs[MAXBITS + 1];
    memset(h->count, 0, sizeof h->count);
    for (int s = 0; s < n; s++)
        h->count[length[s]]++;
    if (h->count[0] == n)
        return 0;
    int left = 1;
    for (int len = 1; len <= MAXBITS; len++) {
        left = (left << 1) - h->count[len];
        if (left < 0)
            return -1;
    }
    offs[1] = 0;
    for (int len = 1; len < MAXBITS; len++)
        offs[len + 1] = offs[len] + h->count[len];
    for (int s = 0; s < n; s++)
        if (length[s])
            h->symbol[offs[length[s]]++] = (short)s;
    return left;
}

static int decode(struct bits *b, const struct huff *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= MAXBITS; len++) {
        int bit = getbits(b, 1);
        if (bit < 0)
            return -1;
        code |= bit;
        int count = h->count[len];
        if (code - count < first)
            return h->symbol[index + code - first];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

static int out_byte(struct bits *b, int c)
{
    if (b->outlen >= b->cap)
        return -1;
    b->out[b->outlen++] = (uint8_t)c;
    return 0;
}

static int codes(struct bits *b, const struct huff *lc, const struct huff *dc)
{
    for (;;) {
        int sym = decode(b, lc);
        if (sym < 0)
            return -1;
        if (sym < 256) {
            if (out_byte(b, sym) < 0)
                return -1;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym < 0 || sym >= 29)
                return -1;
            int extra = getbits(b, lext[sym]);
            if (extra < 0)
                return -1;
            int length = lbase[sym] + extra;
            int ds = decode(b, dc);
            if (ds < 0 || ds >= 30)
                return -1;
            extra = getbits(b, dext[ds]);
            if (extra < 0)
                return -1;
            size_t distance = (size_t)dbase[ds] + (size_t)extra;
            if (distance > b->outlen)
                return -1;
            while (length--)
                if (out_byte(b, b->out[b->outlen - distance]) < 0)
                    return -1;
        }
    }
}

static int stored(struct bits *b)
{
    b->buf = 0;
    b->nbits = 0;
    if (b->pos + 4 > b->len)
        return -1;
    unsigned n = b->in[b->pos] | b->in[b->pos + 1] << 8;
    unsigned inv = b->in[b->pos + 2] | b->in[b->pos + 3] << 8;
    b->pos += 4;
    if (n != (~inv & 0xffff) || b->pos + n > b->len || b->outlen + n > b->cap)
        return -1;
    memcpy(b->out + b->outlen, b->in + b->pos, n);
    b->pos += n;
    b->outlen += n;
    return 0;
}

static int fixed(struct bits *b)
{
    struct huff lc, dc;
    short lengths[FIXLCODES];
    int s = 0;
    for (; s < 144; s++) lengths[s] = 8;
    for (; s < 256; s++) lengths[s] = 9;
    for (; s < 280; s++) lengths[s] = 7;
    for (; s < FIXLCODES; s++) lengths[s] = 8;
    build(&lc, lengths, FIXLCODES);
    for (s = 0; s < MAXDCODES; s++) lengths[s] = 5;
    build(&dc, lengths, MAXDCODES);
    return codes(b, &lc, &dc);
}

static int dynamic(struct bits *b)
{
    static const short order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    short lengths[MAXLCODES + MAXDCODES];
    struct huff lc, dc, ch;
    int nlen = getbits(b, 5), ndist = getbits(b, 5), ncode = getbits(b, 4);
    if (nlen < 0 || ndist < 0 || ncode < 0)
        return -1;
    nlen += 257;
    ndist += 1;
    ncode += 4;
    if (nlen > MAXLCODES || ndist > MAXDCODES)
        return -1;
    memset(lengths, 0, 19 * sizeof *lengths);
    for (int i = 0; i < ncode; i++) {
        int v = getbits(b, 3);
        if (v < 0)
            return -1;
        lengths[order[i]] = (short)v;
    }
    if (build(&ch, lengths, 19) != 0)
        return -1;
    int i = 0;
    while (i < nlen + ndist) {
        int sym = decode(b, &ch);
        if (sym < 0)
            return -1;
        if (sym < 16) {
            lengths[i++] = (short)sym;
            continue;
        }
        int value = 0, repeat;
        if (sym == 16) {
            if (!i)
                return -1;
            value = lengths[i - 1];
            int v = getbits(b, 2);
            if (v < 0) return -1;
            repeat = 3 + v;
        } else if (sym == 17) {
            int v = getbits(b, 3);
            if (v < 0) return -1;
            repeat = 3 + v;
        } else if (sym == 18) {
            int v = getbits(b, 7);
            if (v < 0) return -1;
            repeat = 11 + v;
        } else {
            return -1;
        }
        if (i + repeat > nlen + ndist)
            return -1;
        while (repeat--)
            lengths[i++] = (short)value;
    }
    if (!lengths[256])
        return -1;
    int r = build(&lc, lengths, nlen);
    if (r < 0 || (r > 0 && nlen - lc.count[0] != 1))
        return -1;
    r = build(&dc, lengths + nlen, ndist);
    if (r < 0 || (r > 0 && ndist - dc.count[0] != 1))
        return -1;
    return codes(b, &lc, &dc);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int gunzip_data(const uint8_t *src, size_t len, uint8_t **dst, size_t *dstlen)
{
    if (len < 18 || src[0] != 0x1f || src[1] != 0x8b || src[2] != 8 || (src[3] & 0xe0))
        return -1;
    size_t at = 10;
    int flags = src[3];
    if (flags & 4) {
        if (at + 2 > len - 8) return -1;
        size_t extra = src[at] | (size_t)src[at + 1] << 8;
        at += 2;
        if (at + extra > len - 8) return -1;
        at += extra;
    }
    if (flags & 8) {
        while (at < len - 8 && src[at++]) ;
        if (at > len - 8 || src[at - 1]) return -1;
    }
    if (flags & 16) {
        while (at < len - 8 && src[at++]) ;
        if (at > len - 8 || src[at - 1]) return -1;
    }
    if (flags & 2) {
        if (at + 2 > len - 8) return -1;
        at += 2;
    }
    uint32_t expected = get32(src + len - 4);
    uint8_t *out = malloc(expected ? expected : 1);
    if (!out)
        return -1;
    struct bits b = { .in = src + at, .len = len - at - 8,
                      .out = out, .cap = expected };
    int last;
    do {
        last = getbits(&b, 1);
        int type = getbits(&b, 2);
        if (last < 0 || type < 0) {
            free(out);
            return -1;
        }
        int r = type == 0 ? stored(&b) : type == 1 ? fixed(&b) :
                type == 2 ? dynamic(&b) : -1;
        if (r < 0) {
            free(out);
            return -1;
        }
    } while (!last);
    if (b.outlen != expected || crc32_data(out, b.outlen) != get32(src + len - 8)) {
        free(out);
        return -1;
    }
    *dst = out;
    *dstlen = b.outlen;
    return 0;
}

struct writer {
    uint8_t *data;
    size_t len, cap;
    uint32_t bits;
    int nbits;
};

static int append(struct writer *w, int byte)
{
    if (w->len == w->cap) {
        size_t cap = w->cap ? w->cap * 2 : 1024;
        uint8_t *p = realloc(w->data, cap);
        if (!p)
            return -1;
        w->data = p;
        w->cap = cap;
    }
    w->data[w->len++] = (uint8_t)byte;
    return 0;
}

static int putbits(struct writer *w, unsigned value, int n)
{
    w->bits |= value << w->nbits;
    w->nbits += n;
    while (w->nbits >= 8) {
        if (append(w, w->bits & 0xff) < 0)
            return -1;
        w->bits >>= 8;
        w->nbits -= 8;
    }
    return 0;
}

static unsigned reverse_bits(unsigned value, int n)
{
    unsigned out = 0;
    while (n--) {
        out = (out << 1) | (value & 1);
        value >>= 1;
    }
    return out;
}

static int fixed_symbol(struct writer *w, int sym)
{
    unsigned code;
    int n;
    if (sym <= 143) { code = 0x30u + (unsigned)sym; n = 8; }
    else if (sym <= 255) { code = 0x190u + (unsigned)(sym - 144); n = 9; }
    else if (sym <= 279) { code = (unsigned)(sym - 256); n = 7; }
    else { code = 0xc0u + (unsigned)(sym - 280); n = 8; }
    return putbits(w, reverse_bits(code, n), n);
}

static int emit_match(struct writer *w, int length, int distance)
{
    int ls = 0;
    while (ls < 28 && length > lbase[ls] + ((1 << lext[ls]) - 1))
        ls++;
    if (fixed_symbol(w, 257 + ls) < 0 ||
        putbits(w, (unsigned)(length - lbase[ls]), lext[ls]) < 0)
        return -1;
    int ds = 0;
    while (ds < 29 && distance > dbase[ds] + ((1 << dext[ds]) - 1))
        ds++;
    if (putbits(w, reverse_bits((unsigned)ds, 5), 5) < 0 ||
        putbits(w, (unsigned)(distance - dbase[ds]), dext[ds]) < 0)
        return -1;
    return 0;
}

static unsigned hash3(const uint8_t *p)
{
    return ((unsigned)p[0] * 251u ^ (unsigned)p[1] * 31u ^ p[2]) & 0xffffu;
}

static void insert_pos(const uint8_t *src, size_t len, size_t pos, int *head, int *prev)
{
    if (pos + 2 >= len)
        return;
    unsigned h = hash3(src + pos);
    prev[pos] = head[h];
    head[h] = (int)pos;
}

static int gzip_data(const uint8_t *src, size_t len, uint8_t **dst, size_t *dstlen)
{
    struct writer w = {0};
    static const uint8_t header[10] = { 0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 255 };
    for (size_t i = 0; i < sizeof header; i++)
        if (append(&w, header[i]) < 0)
            return -1;
    if (putbits(&w, 1, 1) < 0 || putbits(&w, 1, 2) < 0)
        return -1;

    int *head = malloc(65536 * sizeof *head);
    int *prev = malloc((len ? len : 1) * sizeof *prev);
    if (!head || !prev) {
        free(head); free(prev); free(w.data);
        return -1;
    }
    for (int i = 0; i < 65536; i++)
        head[i] = -1;
    size_t pos = 0;
    while (pos < len) {
        int best_len = 0, best_dist = 0;
        if (pos + 2 < len) {
            int candidate = head[hash3(src + pos)];
            int attempts = 64;
            while (candidate >= 0 && attempts--) {
                int distance = (int)pos - candidate;
                if (distance > 32768)
                    break;
                int match = 0;
                int max = (int)(len - pos);
                if (max > 258) max = 258;
                while (match < max && src[candidate + match] == src[pos + match])
                    match++;
                if (match >= 3 && match > best_len) {
                    best_len = match;
                    best_dist = distance;
                    if (match == 258)
                        break;
                }
                candidate = prev[candidate];
            }
        }
        if (best_len >= 3) {
            if (emit_match(&w, best_len, best_dist) < 0)
                goto fail;
            for (int i = 0; i < best_len; i++)
                insert_pos(src, len, pos + (size_t)i, head, prev);
            pos += (size_t)best_len;
        } else {
            if (fixed_symbol(&w, src[pos]) < 0)
                goto fail;
            insert_pos(src, len, pos, head, prev);
            pos++;
        }
    }
    if (fixed_symbol(&w, 256) < 0)
        goto fail;
    if (w.nbits && append(&w, w.bits & 0xff) < 0)
        goto fail;
    w.bits = 0;
    w.nbits = 0;
    uint32_t crc = crc32_data(src, len);
    uint32_t size = (uint32_t)len;
    for (int i = 0; i < 4; i++)
        if (append(&w, crc >> (8 * i)) < 0)
            goto fail;
    for (int i = 0; i < 4; i++)
        if (append(&w, size >> (8 * i)) < 0)
            goto fail;
    free(head);
    free(prev);
    *dst = w.data;
    *dstlen = w.len;
    return 0;
fail:
    free(head);
    free(prev);
    free(w.data);
    return -1;
}

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

static int convert(const char *path, int decompress, int stdout_mode, int keep, int test)
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
    int r = decompress ? gunzip_data(src, srclen, &dst, &dstlen) :
                         gzip_data(src, srclen, &dst, &dstlen);
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
        } else if (!keep) {
            unlink(path);
        }
    }
    free(dst);
    return r < 0 ? 1 : 0;
}

int main(int argc, char **argv)
{
    int decompress = 0, stdout_mode = 0, keep = 0, test = 0, i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (strcmp(argv[i], "--") == 0) { i++; break; }
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'c') stdout_mode = 1;
            else if (*p == 'd') decompress = 1;
            else if (*p == 'k') keep = 1;
            else if (*p == 't') { test = 1; decompress = 1; }
            else {
                fprintf(stderr, "usage: gzip [-cdkt] [file...]\n");
                return 2;
            }
        }
    }
    if (i == argc)
        return convert(NULL, decompress, 1, 1, test);
    int status = 0;
    for (; i < argc; i++)
        status |= convert(argv[i], decompress, stdout_mode, keep, test);
    return status;
}
