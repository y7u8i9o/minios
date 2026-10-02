/* The Ogg container (RFC 3533). A page starts with "OggS", a version of 0,
 * the header type flags (1 continued packet, 2 first page of a logical
 * stream, 4 last page), the granule position, the serial number of the
 * logical stream, the page sequence number, a CRC-32 over the whole page
 * with the CRC field set to zero, and the lacing values, whose sum is the
 * length of the body. A packet is a run of lacing values of 255 ended by
 * one below 255, and it may continue on the next page of its stream. */
#include <codec/codec.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_HEADER 27
#define FLAG_CONTINUED 1
#define FLAG_BOS 2
#define FLAG_EOS 4

static uint32_t crc_table[256];
static int crc_ready;

uint32_t codec_ogg_crc(const uint8_t *p, size_t n)
{
    /* The table is computed on first use. Two threads that compute it at
     * the same time write the same values. */
    if (!__atomic_load_n(&crc_ready, __ATOMIC_ACQUIRE)) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i << 24;
            for (int b = 0; b < 8; b++)
                c = c & 0x80000000u ? (c << 1) ^ 0x04c11db7u : c << 1;
            crc_table[i] = c;
        }
        __atomic_store_n(&crc_ready, 1, __ATOMIC_RELEASE);
    }
    uint32_t crc = 0;
    for (size_t i = 0; i < n; i++)
        crc = crc << 8 ^ crc_table[(crc >> 24) ^ p[i]];
    return crc;
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

struct page {
    size_t size;                        /* header and body */
    unsigned flags, segments;
    int64_t granule;
    uint32_t serial;
    const uint8_t *lacing, *body;
};

/* The page at data[at]. Returns 1, 0 when fewer bytes than a page header
 * remain, or -EBADMSG for a page that is cut, has a wrong version or fails
 * its CRC. */
static int parse_page(const uint8_t *d, size_t len, size_t at, struct page *pg)
{
    if (len - at < PAGE_HEADER)
        return 0;
    const uint8_t *h = d + at;
    if (memcmp(h, "OggS", 4) != 0 || h[4] != 0)
        return -EBADMSG;
    unsigned segments = h[26];
    if (len - at < PAGE_HEADER + (size_t)segments)
        return -EBADMSG;
    size_t body = 0;
    for (unsigned i = 0; i < segments; i++)
        body += h[PAGE_HEADER + i];
    size_t size = PAGE_HEADER + segments + body;
    if (len - at < size)
        return -EBADMSG;
    uint8_t copy[PAGE_HEADER];
    memcpy(copy, h, PAGE_HEADER);
    memset(copy + 22, 0, 4);
    uint32_t crc = codec_ogg_crc(copy, PAGE_HEADER);
    /* Continue the CRC over the rest of the page. */
    for (size_t i = PAGE_HEADER; i < size; i++)
        crc = crc << 8 ^ crc_table[(crc >> 24) ^ h[i]];
    if (crc != le32(h + 22))
        return -EBADMSG;
    pg->size = size;
    pg->flags = h[5];
    pg->granule = (int64_t)((uint64_t)le32(h + 6) | (uint64_t)le32(h + 10) << 32);
    pg->serial = le32(h + 14);
    pg->segments = segments;
    pg->lacing = h + PAGE_HEADER;
    pg->body = h + PAGE_HEADER + segments;
    return 1;
}

/* The first packet of a first page, or the part of it on the page. */
static size_t page_first_packet(const struct page *pg, const uint8_t **packet)
{
    size_t n = 0;
    for (unsigned i = 0; i < pg->segments; i++) {
        n += pg->lacing[i];
        if (pg->lacing[i] < 255)
            break;
    }
    *packet = pg->body;
    return n;
}

static int accepted(const struct page *pg, int (*accept)(const uint8_t *, size_t))
{
    const uint8_t *packet;
    size_t n = page_first_packet(pg, &packet);
    return (pg->flags & FLAG_BOS) && accept(packet, n);
}

void codec_ogg_reader_init(struct codec_ogg_reader *r, const uint8_t *data, size_t len,
                           int (*accept)(const uint8_t *packet, size_t len))
{
    memset(r, 0, sizeof *r);
    r->data = data;
    r->len = len;
    r->accept = accept;
}

void codec_ogg_reader_free(struct codec_ogg_reader *r)
{
    free(r->packet);
    r->packet = NULL;
}

/* Find the next page of the followed stream and make it current. Returns
 * 1, 0 at the end of the data, or -EBADMSG. A page that is cut or fails
 * its CRC is damage, and the data must not end before the last page of
 * the followed stream. Bytes after the last page that hold no page
 * header, such as a tag, end the data. */
static int load_page(struct codec_ogg_reader *r)
{
    for (;;) {
        struct page pg;
        int rc = parse_page(r->data, r->len, r->next, &pg);
        if (rc < 0 && (r->len - r->next < 4 || memcmp(r->data + r->next, "OggS", 4) != 0)) {
            /* No page header here: damage when a valid page follows,
             * else the end of the data. */
            rc = 0;
            for (size_t at = r->next + 1; at + PAGE_HEADER <= r->len; at++)
                if (r->data[at] == 'O' && parse_page(r->data, r->len, at, &pg) == 1)
                    return -EBADMSG;
        }
        if (rc < 0)
            return -EBADMSG;
        if (rc == 0)
            return r->have_serial && !r->ended ? -EBADMSG : 0;
        r->next += pg.size;
        if (!r->have_serial) {
            if (!accepted(&pg, r->accept))
                continue;
            r->serial = pg.serial;
            r->have_serial = 1;
        } else if (pg.serial != r->serial || r->ended) {
            /* After the end of the followed stream, a new accepted stream
             * continues the chain. */
            if (!r->ended || !accepted(&pg, r->accept))
                continue;
            r->serial = pg.serial;
            r->ended = 0;
            r->packet_len = 0;
        }
        r->lacing = pg.lacing;
        r->body = pg.body;
        r->segments = pg.segments;
        r->segment = 0;
        r->body_at = 0;
        r->granule = pg.granule;
        r->page_eos = (pg.flags & FLAG_EOS) != 0;
        r->page_bos = (pg.flags & FLAG_BOS) != 0;
        r->last_end = UINT32_MAX;
        for (unsigned i = 0; i < pg.segments; i++)
            if (pg.lacing[i] < 255)
                r->last_end = i;
        if (r->page_bos)
            r->packet_bos = 1;
        if (pg.flags & FLAG_CONTINUED) {
            if (!r->packet_len) {
                /* The start of this packet is missing: skip its rest. */
                while (r->segment < r->segments) {
                    unsigned lace = r->lacing[r->segment++];
                    r->body_at += lace;
                    if (lace < 255)
                        break;
                }
            }
        } else if (r->packet_len) {
            return -EBADMSG;            /* the previous packet was not finished */
        }
        return 1;
    }
}

int codec_ogg_next(struct codec_ogg_reader *r, struct codec_ogg_packet *p)
{
    if (r->packet_len && r->packet_len == (size_t)-1)
        r->packet_len = 0;
    for (;;) {
        while (r->lacing && r->segment < r->segments) {
            unsigned index = r->segment++, lace = r->lacing[index];
            if (r->packet_len + lace > r->packet_cap) {
                size_t cap = r->packet_cap ? r->packet_cap * 2 : 4096;
                while (cap < r->packet_len + lace)
                    cap *= 2;
                uint8_t *grown = realloc(r->packet, cap);
                if (!grown)
                    return -ENOMEM;
                r->packet = grown;
                r->packet_cap = cap;
            }
            memcpy(r->packet + r->packet_len, r->body + r->body_at, lace);
            r->packet_len += lace;
            r->body_at += lace;
            if (lace == 255)
                continue;
            p->data = r->packet;
            p->len = r->packet_len;
            p->granule = index == r->last_end ? r->granule : -1;
            p->eos = r->page_eos && index == r->last_end;
            p->bos = r->packet_bos;
            p->serial = r->serial;
            r->packet_bos = 0;
            r->packet_len = (size_t)-1;     /* reset at the next call */
            if (p->eos)
                r->ended = 1;
            return 1;
        }
        int rc = load_page(r);
        if (rc <= 0)
            return rc;
    }
}

size_t codec_ogg_first_packet(const uint8_t *data, size_t len, int (*accept)(const uint8_t *packet, size_t len),
                              const uint8_t **packet)
{
    struct page pg;
    for (size_t at = 0; parse_page(data, len, at, &pg) == 1 && (pg.flags & FLAG_BOS); at += pg.size)
        if (accepted(&pg, accept))
            return page_first_packet(&pg, packet);
    return 0;
}

int64_t codec_ogg_total_granule(const uint8_t *data, size_t len, int (*accept)(const uint8_t *packet, size_t len))
{
    struct page pg;
    int64_t total = -1, chain = -1;
    uint32_t serial = 0;
    int have = 0;
    for (size_t at = 0; parse_page(data, len, at, &pg) == 1; at += pg.size) {
        if (accepted(&pg, accept)) {
            if (have && chain > 0)
                total = (total < 0 ? 0 : total) + chain;
            serial = pg.serial;
            have = 1;
            chain = -1;
        } else if (!have || pg.serial != serial) {
            continue;
        }
        if (pg.granule >= 0)
            chain = pg.granule;
    }
    if (have && chain >= 0)
        total = (total < 0 ? 0 : total) + chain;
    return total;
}
