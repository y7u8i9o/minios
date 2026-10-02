/* Inflate (RFC 1951) with zlib framing (RFC 1950): stored, fixed and
 * dynamic Huffman blocks, decoded with canonical code tables. Shared by
 * the modules, as zlib is on Linux. */
#include <codec/codec.h>
#include <string.h>
#include <errno.h>

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
    if (n == 0)
        return 0;
    if (need(b, n) < 0)
        return -1;
    int v = (int)(b->buf & ((1u << n) - 1));
    b->buf >>= n;
    b->nbits -= n;
    return v;
}

#define MAXBITS 15
#define MAXLCODES 286
#define MAXDCODES 30
#define FIXLCODES 288

struct huff {
    short count[MAXBITS + 1];
    short symbol[FIXLCODES];
};

/* Build canonical decoding tables from code lengths; returns 0 when the
 * code is complete, positive when incomplete (allowed for one code). */
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
        left <<= 1;
        left -= h->count[len];
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
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static int put(struct bits *b, int c)
{
    if (b->outlen >= b->cap)
        return -1;
    b->out[b->outlen++] = (uint8_t)c;
    return 0;
}

static const short lbase[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const short lext[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const short dbase[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const short dext[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static int codes(struct bits *b, const struct huff *lc, const struct huff *dc)
{
    for (;;) {
        int sym = decode(b, lc);
        if (sym < 0)
            return -1;
        if (sym < 256) {
            if (put(b, sym) < 0)
                return -1;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29)
                return -1;
            int e = getbits(b, lext[sym]);
            if (e < 0)
                return -1;
            int len = lbase[sym] + e;
            int ds = decode(b, dc);
            if (ds < 0 || ds >= 30)
                return -1;
            e = getbits(b, dext[ds]);
            if (e < 0)
                return -1;
            size_t dist = (size_t)dbase[ds] + (size_t)e;
            if (dist > b->outlen)
                return -1;
            while (len--) {
                if (put(b, b->out[b->outlen - dist]) < 0)
                    return -1;
            }
        }
    }
}

static int stored(struct bits *b)
{
    b->buf = 0;
    b->nbits = 0;
    if (b->pos + 4 > b->len)
        return -1;
    unsigned len = b->in[b->pos] | b->in[b->pos + 1] << 8;
    unsigned nlen = b->in[b->pos + 2] | b->in[b->pos + 3] << 8;
    b->pos += 4;
    if (len != (~nlen & 0xffff) || b->pos + len > b->len || b->outlen + len > b->cap)
        return -1;
    memcpy(b->out + b->outlen, b->in + b->pos, len);
    b->pos += len;
    b->outlen += len;
    return 0;
}

static int fixed(struct bits *b)
{
    static struct huff lc, dc;
    static int built;
    if (!built) {
        short lengths[FIXLCODES];
        int s = 0;
        for (; s < 144; s++) lengths[s] = 8;
        for (; s < 256; s++) lengths[s] = 9;
        for (; s < 280; s++) lengths[s] = 7;
        for (; s < FIXLCODES; s++) lengths[s] = 8;
        build(&lc, lengths, FIXLCODES);
        for (s = 0; s < MAXDCODES; s++) lengths[s] = 5;
        build(&dc, lengths, MAXDCODES);
        built = 1;
    }
    return codes(b, &lc, &dc);
}

static int dynamic(struct bits *b)
{
    static const short order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    short lengths[MAXLCODES + MAXDCODES];
    struct huff lc, dc;
    int nlen = getbits(b, 5) + 257, ndist = getbits(b, 5) + 1, ncode = getbits(b, 4) + 4;
    if (nlen > MAXLCODES || ndist > MAXDCODES)
        return -1;
    for (int i = 0; i < 19; i++)
        lengths[order[i]] = 0;
    for (int i = 0; i < ncode; i++) {
        int v = getbits(b, 3);
        if (v < 0)
            return -1;
        lengths[order[i]] = (short)v;
    }
    if (build(&lc, lengths, 19) != 0)
        return -1;
    int i = 0;
    while (i < nlen + ndist) {
        int sym = decode(b, &lc);
        if (sym < 0)
            return -1;
        if (sym < 16) {
            lengths[i++] = (short)sym;
            continue;
        }
        int len = 0, rep;
        if (sym == 16) {
            if (i == 0)
                return -1;
            len = lengths[i - 1];
            rep = 3 + getbits(b, 2);
        } else if (sym == 17) {
            rep = 3 + getbits(b, 3);
        } else {
            rep = 11 + getbits(b, 7);
        }
        if (i + rep > nlen + ndist)
            return -1;
        while (rep--)
            lengths[i++] = (short)len;
    }
    if (lengths[256] == 0)
        return -1;
    int r = build(&lc, lengths, nlen);
    if (r < 0 || (r > 0 && nlen - lc.count[0] != 1))
        return -1;
    r = build(&dc, lengths + nlen, ndist);
    if (r < 0 || (r > 0 && ndist - dc.count[0] != 1))
        return -1;
    return codes(b, &lc, &dc);
}

long codec_inflate(uint8_t *dst, size_t cap, const uint8_t *src, size_t len)
{
    if (len < 6)
        return -EINVAL;
    int cmf = src[0], flg = src[1];
    if ((cmf & 0x0f) != 8 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20))
        return -EINVAL;
    struct bits b = { .in = src + 2, .len = len - 6, .out = dst, .cap = cap };
    int last;
    do {
        last = getbits(&b, 1);
        int type = getbits(&b, 2);
        if (last < 0 || type < 0)
            return -EINVAL;
        int r = type == 0 ? stored(&b) : type == 1 ? fixed(&b) : type == 2 ? dynamic(&b) : -1;
        if (r < 0)
            return -EINVAL;
    } while (!last);
    /* Adler-32 of the output against the trailer. */
    uint32_t a = 1, s = 0;
    for (size_t i = 0; i < b.outlen; i++) {
        a = (a + dst[i]) % 65521;
        s = (s + a) % 65521;
    }
    const uint8_t *t = src + len - 4;
    uint32_t want = (uint32_t)t[0] << 24 | (uint32_t)t[1] << 16 | (uint32_t)t[2] << 8 | t[3];
    if (((s << 16) | a) != want)
        return -EINVAL;
    return (long)b.outlen;
}
