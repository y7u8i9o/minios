/* FLAC in Ogg, after the Ogg mapping of FLAC 1.0. The first packet of a
 * logical stream contains 0x7f and "FLAC", the mapping version 1.0, the
 * number of header packets that follow it (0 when unknown), the "fLaC"
 * marker and the STREAMINFO block with its header. Each further header
 * packet contains one metadata block, and each audio packet one frame. The
 * granule position of a page is the number of samples up to the end of
 * the last frame completed on it.
 *
 * The decoder reads the whole file through the Ogg reader of libcodec,
 * reassembles every logical stream it accepts into a native FLAC stream
 * of the marker, STREAMINFO and the frames, and decodes the streams one
 * after another with the native decoder, which checks the CRCs and the
 * MD5 sum of each. Chained streams must have the format of the first. */
#include "flac.h"

#define FIRST_PACKET 51

static int is_oggflac(const uint8_t *p, size_t len)
{
    return len >= FIRST_PACKET && p[0] == 0x7f && memcmp(p + 1, "FLAC", 4) == 0 && p[5] == 1 &&
           memcmp(p + 9, "fLaC", 4) == 0 && (p[13] & 0x7f) == 0;
}

int oggflac_probe(const uint8_t *data, size_t len)
{
    return codec_ogg_probe(data, len, is_oggflac);
}

struct chain {
    uint8_t *data;
    size_t len, cap;
};

struct oggflac_state {
    struct chain *chains;
    int nchains, current;
    void *inner;
    struct codec_audio_format fmt;
    int error;                          /* from the Ogg reader, reported after the chains */
};

static int append(struct chain *c, const uint8_t *p, size_t n)
{
    if (c->len + n > c->cap) {
        size_t cap = c->cap ? c->cap * 2 : 65536;
        while (cap < c->len + n)
            cap *= 2;
        uint8_t *g = realloc(c->data, cap);
        if (!g)
            return -ENOMEM;
        c->data = g;
        c->cap = cap;
    }
    memcpy(c->data + c->len, p, n);
    c->len += n;
    return 0;
}

void oggflac_close(void *state)
{
    struct oggflac_state *s = state;
    if (!s)
        return;
    if (s->inner)
        flac_close(s->inner);
    for (int i = 0; i < s->nchains; i++)
        free(s->chains[i].data);
    free(s->chains);
    free(s);
}

/* Open the native decoder for chain i. */
static int open_chain(struct oggflac_state *s, int i, struct codec_audio_format *fmt, long *frames)
{
    if (s->inner)
        flac_close(s->inner);
    s->inner = NULL;
    s->current = i;
    return flac_open(s->chains[i].data, s->chains[i].len, fmt, frames, &s->inner);
}

int oggflac_open(const uint8_t *data, size_t len, struct codec_audio_format *fmt, long *frames, void **state)
{
    struct oggflac_state *s = calloc(1, sizeof *s);
    if (!s)
        return -ENOMEM;
    struct codec_ogg_reader r;
    codec_ogg_reader_init(&r, data, len, is_oggflac);
    struct codec_ogg_packet p;
    int rc, headers = 0, counted = 0, err = 0;
    while ((rc = codec_ogg_next(&r, &p)) > 0 && !err) {
        if (p.bos) {
            struct chain *g = realloc(s->chains, sizeof *g * (size_t)(s->nchains + 1));
            if (!g) {
                err = -ENOMEM;
                break;
            }
            s->chains = g;
            struct chain *c = &s->chains[s->nchains++];
            memset(c, 0, sizeof *c);
            /* The marker and STREAMINFO, marked as the last metadata block. */
            uint8_t head[8] = { 'f', 'L', 'a', 'C', 0x80, 0, 0, FLAC_STREAMINFO_LEN };
            err = append(c, head, sizeof head);
            if (!err)
                err = append(c, p.data + 17, FLAC_STREAMINFO_LEN);
            headers = p.data[7] << 8 | p.data[8];
            counted = headers != 0;
            continue;
        }
        if (!s->nchains)
            continue;
        int frame = p.len >= 2 && p.data[0] == 0xff && (p.data[1] & 0xfe) == 0xf8;
        if (counted ? headers > 0 : !frame) {
            headers -= counted;         /* a metadata block, which the native decoder would skip */
            continue;
        }
        err = append(&s->chains[s->nchains - 1], p.data, p.len);
    }
    codec_ogg_reader_free(&r);
    s->error = rc < 0 ? rc : 0;
    if (err || !s->nchains) {
        oggflac_close(s);
        return err ? err : -EINVAL;
    }
    /* Every chain must open and have the format of the first. The length
     * is the sum of the totals of STREAMINFO, unknown when one is 0. */
    long total = 0;
    for (int i = s->nchains - 1; i >= 0; i--) {
        struct codec_audio_format f;
        long n;
        err = open_chain(s, i, &f, &n);
        if (err || (i < s->nchains - 1 && (f.rate != s->fmt.rate || f.channels != s->fmt.channels ||
                                            f.bits != s->fmt.bits))) {
            if (i == 0) {
                oggflac_close(s);
                return err ? err : -EINVAL;
            }
            s->nchains = i;             /* end the file before a chain of another format */
            total = 0;
            continue;
        }
        s->fmt = f;
        total = n < 0 || total < 0 ? -1 : total + n;
    }
    /* Without totals in STREAMINFO, as ffmpeg writes it, the granule
     * positions give the length. */
    if (total < 0) {
        int64_t granules = codec_ogg_total_granule(data, len, is_oggflac);
        total = granules >= 0 ? (long)granules : -1;
    }
    *fmt = s->fmt;
    *frames = total;
    *state = s;
    return 0;
}

long oggflac_read(void *state, int32_t *out, long frames)
{
    struct oggflac_state *s = state;
    long done = 0;
    while (done < frames) {
        long n = flac_read(s->inner, out + done * s->fmt.channels, frames - done);
        if (n < 0)
            return done ? done : n;
        if (n > 0) {
            done += n;
            continue;
        }
        if (s->current + 1 >= s->nchains)
            return done ? done : s->error;
        struct codec_audio_format f;
        long total;
        int err = open_chain(s, s->current + 1, &f, &total);
        if (err)
            return done ? done : err;
    }
    return done;
}

/* ---- encoding ---- */

long oggflac_encode(const struct codec_audio_format *fmt, const int32_t *samples, long frames, uint8_t **result)
{
    struct flac_encoded e;
    int err = flac_encode_stream(fmt, samples, frames, &e);
    if (err)
        return err;
    struct codec_ogg_writer w;
    codec_ogg_writer_init(&w, 0x666c6163u ^ (uint32_t)frames);
    /* The first packet with one header packet to follow, STREAMINFO not
     * marked as the last block. */
    uint8_t first[FIRST_PACKET] = { 0x7f, 'F', 'L', 'A', 'C', 1, 0, 0, 1, 'f', 'L', 'a', 'C', 0, 0, 0,
                                    FLAC_STREAMINFO_LEN };
    memcpy(first + 17, e.streaminfo, FLAC_STREAMINFO_LEN);
    codec_ogg_write_packet(&w, first, sizeof first, 0);
    codec_ogg_flush(&w, 0);
    /* The VORBIS_COMMENT block that the mapping requires as the second
     * header packet: the vendor string and no comments. */
    static const char vendor[] = "minios libcodec";
    uint8_t comment[4 + 4 + sizeof vendor - 1 + 4];
    size_t body = sizeof comment - 4;
    comment[0] = 0x84;                  /* the last block, VORBIS_COMMENT */
    comment[1] = (uint8_t)(body >> 16);
    comment[2] = (uint8_t)(body >> 8);
    comment[3] = (uint8_t)body;
    comment[4] = (uint8_t)(sizeof vendor - 1);
    comment[5] = comment[6] = comment[7] = 0;
    memcpy(comment + 8, vendor, sizeof vendor - 1);
    memset(comment + 8 + sizeof vendor - 1, 0, 4);
    codec_ogg_write_packet(&w, comment, sizeof comment, 0);
    codec_ogg_flush(&w, 0);
    int64_t granule = 0;
    for (long i = 0; i < e.count; i++) {
        size_t start = i ? e.ends[i - 1] : 0;
        granule += e.samples[i];
        codec_ogg_write_packet(&w, e.frames + start, e.ends[i] - start, granule);
    }
    codec_ogg_flush(&w, 1);
    flac_encoded_free(&e);
    if (w.failed) {
        codec_ogg_writer_free(&w);
        return -ENOMEM;
    }
    free(w.body);
    *result = w.out;
    return (long)w.len;
}
