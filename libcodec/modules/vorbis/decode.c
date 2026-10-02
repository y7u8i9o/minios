/* Decoding of Ogg Vorbis streams. The Ogg reader of libcodec returns the
 * packets of the first Vorbis stream in the file and of every chained
 * Vorbis stream after it with the same rate and channel count. Each
 * stream starts with its three header packets. An audio packet produces
 * the samples between the centres of the previous and the current block,
 * and the granule positions trim the start and the end of each stream
 * (section 4.3 of the specification). Samples are returned in the channel
 * order of WAV files. */
#include "vorbis.h"
#include <math.h>

struct vb_state {
    struct codec_ogg_reader ogg;
    struct vb_setup setup;
    int headers;                        /* header packets read of the current stream */
    unsigned channels, rate;            /* of the first stream, which later chains must match */
    unsigned maxblock;
    struct vb_mdct mdct[2];
    unsigned mdct_size[2];
    float *ramp[2];                     /* the rising slope of a short and a long window */
    float **coeff, **pcm, **prev;       /* per channel */
    struct vb_floor_data *floor_data;
    int *unused, *no_residue, *skip;
    float *curve, *scratch;
    float **vectors;
    unsigned prev_n;
    int have_prev;
    /* Decoded samples, interleaved in WAV order. The first ready frames may
     * be returned. The others wait for the first granule position of their
     * stream. */
    float *out;
    size_t out_n, out_pos, out_ready, out_cap;
    size_t chain_start;                 /* the first frame of the current stream in out */
    int64_t chain_produced;             /* frames the current stream has produced */
    int granule_seen;
    int ended, error;
    int order[8];                       /* WAV position to Vorbis channel */
};

static int is_vorbis(const uint8_t *p, size_t len)
{
    return len >= 7 && p[0] == 1 && memcmp(p + 1, "vorbis", 6) == 0;
}

/* The Vorbis channel order of section 4.3.9 mapped to the order of WAV
 * files for one to eight channels. */
static void channel_order(int *order, unsigned channels)
{
    static const int maps[9][8] = {
        { 0 }, { 0 }, { 0, 1 }, { 0, 2, 1 }, { 0, 1, 2, 3 }, { 0, 2, 1, 3, 4 }, { 0, 2, 1, 5, 3, 4 },
        { 0, 2, 1, 6, 5, 3, 4 }, { 0, 2, 1, 7, 5, 6, 3, 4 },
    };
    for (unsigned c = 0; c < 8; c++)
        order[c] = channels <= 8 ? maps[channels][c] : (int)c;
}

static void free_buffers(struct vb_state *s)
{
    for (unsigned c = 0; c < s->channels; c++) {
        if (s->coeff)
            free(s->coeff[c]);
        if (s->pcm)
            free(s->pcm[c]);
        if (s->prev)
            free(s->prev[c]);
    }
    free(s->coeff);
    free(s->pcm);
    free(s->prev);
    free(s->floor_data);
    free(s->unused);
    free(s->no_residue);
    free(s->skip);
    free(s->curve);
    free(s->scratch);
    free(s->vectors);
    free(s->ramp[0]);
    free(s->ramp[1]);
    s->coeff = s->pcm = s->prev = s->vectors = NULL;
    s->floor_data = NULL;
    s->unused = s->no_residue = s->skip = NULL;
    s->curve = s->scratch = NULL;
    s->ramp[0] = s->ramp[1] = NULL;
    for (int i = 0; i < 2; i++)
        vb_mdct_free(&s->mdct[i]);
    s->maxblock = 0;
}

/* The buffers for the block sizes of the current setup. */
static int alloc_buffers(struct vb_state *s)
{
    unsigned bs0 = s->setup.blocksize[0], bs1 = s->setup.blocksize[1], ch = s->channels;
    if (s->maxblock == bs1 && s->mdct_size[0] == bs0)
        return 0;
    free_buffers(s);
    s->maxblock = bs1;
    s->coeff = calloc(ch, sizeof *s->coeff);
    s->pcm = calloc(ch, sizeof *s->pcm);
    s->prev = calloc(ch, sizeof *s->prev);
    s->vectors = calloc(ch, sizeof *s->vectors);
    s->floor_data = calloc(ch, sizeof *s->floor_data);
    s->unused = calloc(ch, sizeof *s->unused);
    s->no_residue = calloc(ch, sizeof *s->no_residue);
    s->skip = calloc(ch, sizeof *s->skip);
    s->curve = malloc(sizeof *s->curve * bs1 / 2);
    s->scratch = malloc(sizeof *s->scratch * bs1 / 2 * ch);
    if (!s->coeff || !s->pcm || !s->prev || !s->vectors || !s->floor_data || !s->unused || !s->no_residue ||
        !s->skip || !s->curve || !s->scratch)
        return -ENOMEM;
    for (unsigned c = 0; c < ch; c++) {
        s->coeff[c] = malloc(sizeof **s->coeff * bs1 / 2);
        s->pcm[c] = malloc(sizeof **s->pcm * bs1);
        s->prev[c] = malloc(sizeof **s->prev * bs1);
        if (!s->coeff[c] || !s->pcm[c] || !s->prev[c])
            return -ENOMEM;
    }
    for (int b = 0; b < 2; b++) {
        unsigned n = s->setup.blocksize[b], ramp = n / 2;
        s->mdct_size[b] = n;
        if (vb_mdct_init(&s->mdct[b], n) < 0)
            return -ENOMEM;
        s->ramp[b] = malloc(sizeof *s->ramp[b] * ramp);
        if (!s->ramp[b])
            return -ENOMEM;
        for (unsigned i = 0; i < ramp; i++) {
            double x = sin((i + 0.5) / ramp * M_PI / 2);
            s->ramp[b][i] = (float)sin(M_PI / 2 * x * x);
        }
    }
    return 0;
}

/* Multiply a block of size n by its window. A long block next to a short
 * one uses the short slope on that side, centred on the quarter point. */
static void apply_window(const struct vb_state *s, float *v, unsigned n, unsigned blockflag, unsigned prevflag,
                         unsigned nextflag)
{
    unsigned bs0 = s->setup.blocksize[0];
    unsigned ls, le, rs, re;
    const float *lramp, *rramp;
    if (blockflag && !prevflag) {
        ls = n / 4 - bs0 / 4;
        le = n / 4 + bs0 / 4;
        lramp = s->ramp[0];
    } else {
        ls = 0;
        le = n / 2;
        lramp = s->ramp[blockflag];
    }
    if (blockflag && !nextflag) {
        rs = n * 3 / 4 - bs0 / 4;
        re = n * 3 / 4 + bs0 / 4;
        rramp = s->ramp[0];
    } else {
        rs = n / 2;
        re = n;
        rramp = s->ramp[blockflag];
    }
    for (unsigned i = 0; i < ls; i++)
        v[i] = 0;
    for (unsigned i = ls; i < le; i++)
        v[i] *= lramp[i - ls];
    for (unsigned i = rs; i < re; i++)
        v[i] *= rramp[re - 1 - i];
    for (unsigned i = re; i < n; i++)
        v[i] = 0;
}

static int out_reserve(struct vb_state *s, size_t frames)
{
    size_t need = (s->out_n + frames) * s->channels;
    if (need <= s->out_cap)
        return 0;
    if (s->out_pos) {
        /* Drop the frames that were read. */
        memmove(s->out, s->out + s->out_pos * s->channels, (s->out_n - s->out_pos) * s->channels * sizeof *s->out);
        s->out_n -= s->out_pos;
        s->out_ready -= s->out_pos;
        s->chain_start -= s->out_pos < s->chain_start ? s->out_pos : s->chain_start;
        s->out_pos = 0;
        need = (s->out_n + frames) * s->channels;
        if (need <= s->out_cap)
            return 0;
    }
    size_t cap = s->out_cap ? s->out_cap : 65536;
    while (cap < need)
        cap *= 2;
    float *grown = realloc(s->out, sizeof *grown * cap);
    if (!grown)
        return -ENOMEM;
    s->out = grown;
    s->out_cap = cap;
    return 0;
}

/* Decode one audio packet and append its frames. Returns the number of
 * frames, or 0 for a packet that produces none. */
static long audio_packet(struct vb_state *s, const uint8_t *data, size_t len)
{
    const struct vb_setup *su = &s->setup;
    struct vb_reader r = { data, len, 0, 0 };
    if (vb_bits(&r, 1) != 0 || r.eop)
        return 0;                       /* not an audio packet */
    unsigned mode = vb_bits(&r, vb_ilog(su->nmodes - 1));
    if (mode >= su->nmodes || r.eop)
        return 0;
    unsigned blockflag = su->modes[mode].blockflag, prevflag = 0, nextflag = 0;
    if (blockflag) {
        prevflag = vb_bits(&r, 1);
        nextflag = vb_bits(&r, 1);
    }
    if (r.eop)
        return 0;
    unsigned n = su->blocksize[blockflag], half = n / 2, ch = s->channels;
    const struct vb_mapping *map = &su->mappings[su->modes[mode].mapping];

    for (unsigned c = 0; c < ch; c++) {
        const struct vb_floor *f = &su->floors[map->submap_floor[map->mux[c]]];
        s->unused[c] = vb_floor_decode(su, f, &r, &s->floor_data[c]);
        s->no_residue[c] = s->unused[c];
        memset(s->coeff[c], 0, sizeof **s->coeff * half);
    }
    for (unsigned i = 0; i < map->coupling_steps; i++)
        if (!s->no_residue[map->magnitude[i]] || !s->no_residue[map->angle[i]])
            s->no_residue[map->magnitude[i]] = s->no_residue[map->angle[i]] = 0;
    for (unsigned sm = 0; sm < map->submaps; sm++) {
        unsigned count = 0;
        for (unsigned c = 0; c < ch; c++)
            if (map->mux[c] == sm) {
                s->vectors[count] = s->coeff[c];
                s->skip[count] = s->no_residue[c];
                count++;
            }
        vb_residue_decode(su, &su->residues[map->submap_residue[sm]], &r, s->vectors, s->skip, count, n, s->scratch);
    }
    for (unsigned i = map->coupling_steps; i-- > 0;) {
        float *mv = s->coeff[map->magnitude[i]], *av = s->coeff[map->angle[i]];
        for (unsigned j = 0; j < half; j++) {
            float m = mv[j], a = av[j];
            if (m > 0) {
                if (a > 0) {
                    av[j] = m - a;
                } else {
                    av[j] = m;
                    mv[j] = m + a;
                }
            } else {
                if (a > 0) {
                    av[j] = m + a;
                } else {
                    av[j] = m;
                    mv[j] = m - a;
                }
            }
        }
    }
    for (unsigned c = 0; c < ch; c++) {
        if (s->unused[c]) {
            memset(s->pcm[c], 0, sizeof **s->pcm * n);
            continue;
        }
        const struct vb_floor *f = &su->floors[map->submap_floor[map->mux[c]]];
        vb_floor_render(f, &s->floor_data[c], blockflag, n, s->curve);
        for (unsigned j = 0; j < half; j++)
            s->coeff[c][j] *= s->curve[j];
        vb_imdct(&s->mdct[blockflag], s->coeff[c], s->pcm[c]);
        apply_window(s, s->pcm[c], n, blockflag, prevflag, nextflag);
    }

    long produced = 0;
    if (s->have_prev) {
        /* Overlap and add from the centre of the previous block to the
         * centre of this one. Index c counts in this block, and the
         * previous block lines up with its three quarter point on this
         * block's quarter point. */
        long start = (long)(n / 4) - (long)(s->prev_n / 4), stop = (long)(n / 2);
        produced = stop - start;
        if (out_reserve(s, (size_t)produced) < 0)
            return -ENOMEM;
        float *dst = s->out + s->out_n * ch;
        long shift = (long)(s->prev_n * 3 / 4) - (long)(n / 4);
        for (long c = start; c < stop; c++, dst += ch)
            for (unsigned k = 0; k < ch; k++) {
                unsigned v = k < 8 ? (unsigned)s->order[k] : k;
                float x = c >= 0 ? s->pcm[v][c] : 0;
                long p = c + shift;
                if (p < (long)s->prev_n)
                    x += s->prev[v][p];
                dst[k] = x;
            }
        s->out_n += (size_t)produced;
    }
    float **t = s->prev;
    s->prev = s->pcm;
    s->pcm = t;
    s->prev_n = n;
    s->have_prev = 1;
    return produced;
}

/* Apply the granule position of a packet that ends a page. The first one
 * in a stream fixes how many frames at its start to drop, and the last one
 * how many frames at its end. */
static void apply_granule(struct vb_state *s, int64_t granule, int eos, long produced)
{
    int64_t before = s->chain_produced - produced;
    if (eos && granule < s->chain_produced) {
        int64_t keep = granule - before;
        long cut = keep < 0 ? produced : (long)(produced - keep);
        if (cut > 0) {
            s->out_n -= (size_t)cut;
            s->chain_produced -= cut;
        }
    } else if (!s->granule_seen && granule < s->chain_produced) {
        size_t drop = (size_t)(s->chain_produced - granule), have = s->out_n - s->chain_start;
        if (drop > have)
            drop = have;
        float *at = s->out + s->chain_start * s->channels;
        memmove(at, at + drop * s->channels, (s->out_n - s->chain_start - drop) * s->channels * sizeof *s->out);
        s->out_n -= drop;
    }
    s->granule_seen = 1;
    s->out_ready = s->out_n;
}

/* Read packets until new frames are ready, the stream ends, or an error
 * occurs. */
static int advance(struct vb_state *s)
{
    while (s->out_ready == s->out_pos && !s->ended && !s->error) {
        struct codec_ogg_packet p;
        int rc = codec_ogg_next(&s->ogg, &p);
        if (rc <= 0) {
            if (rc < 0)
                s->error = rc;
            s->ended = 1;
            s->out_ready = s->out_n;    /* a stream without a granule position */
            break;
        }
        if (p.bos) {
            s->headers = 0;
            s->have_prev = 0;
            s->granule_seen = 0;
            s->chain_produced = 0;
            s->out_ready = s->out_n;
            s->chain_start = s->out_n;
        }
        if (s->headers == 0) {
            struct vb_setup id = { 0 };
            if (vb_parse_identification(p.data, p.len, &id) < 0) {
                s->error = -EBADMSG;
                break;
            }
            if (s->channels && (id.channels != s->channels || id.rate != s->rate)) {
                s->ended = 1;           /* a chained stream of another format */
                break;
            }
            vb_setup_free(&s->setup);
            s->setup.channels = id.channels;
            s->setup.rate = id.rate;
            s->setup.blocksize[0] = id.blocksize[0];
            s->setup.blocksize[1] = id.blocksize[1];
            s->headers = 1;
            continue;
        }
        if (s->headers == 1) {
            if (vb_parse_comment(p.data, p.len) < 0) {
                s->error = -EBADMSG;
                break;
            }
            s->headers = 2;
            continue;
        }
        if (s->headers == 2) {
            int err = vb_parse_setup(p.data, p.len, &s->setup);
            if (!err)
                err = alloc_buffers(s);
            if (err) {
                s->error = err == -ENOMEM ? err : -EBADMSG;
                break;
            }
            s->headers = 3;
            continue;
        }
        long produced = audio_packet(s, p.data, p.len);
        if (produced < 0) {
            s->error = (int)produced;
            break;
        }
        s->chain_produced += produced;
        if (p.granule >= 0)
            apply_granule(s, p.granule, p.eos, produced);
        else if (s->granule_seen)
            s->out_ready = s->out_n;
    }
    return s->error;
}

static int vorbis_open(const uint8_t *data, size_t len, struct codec_audio_format *fmt, long *frames, void **state)
{
    struct vb_state *s = calloc(1, sizeof *s);
    if (!s)
        return -ENOMEM;
    codec_ogg_reader_init(&s->ogg, data, len, is_vorbis);
    const uint8_t *id;
    size_t idlen = codec_ogg_first_packet(data, len, is_vorbis, &id);
    struct vb_setup info = { 0 };
    if (!idlen || vb_parse_identification(id, idlen, &info) < 0) {
        free(s);
        return -EINVAL;
    }
    s->channels = info.channels;
    s->rate = info.rate;
    channel_order(s->order, s->channels);
    fmt->rate = (int)info.rate;
    fmt->channels = (int)info.channels;
    fmt->bits = 0;                      /* Vorbis has no sample size */
    int64_t total = codec_ogg_total_granule(data, len, is_vorbis);
    *frames = total >= 0 ? (long)total : -1;
    *state = s;
    return 0;
}

static long vorbis_read(void *state, int32_t *out, long frames)
{
    struct vb_state *s = state;
    long done = 0;
    unsigned ch = s->channels;
    while (done < frames) {
        if (s->out_pos == s->out_ready && advance(s) < 0 && s->out_pos == s->out_ready)
            break;
        if (s->out_pos == s->out_ready)
            break;
        size_t n = s->out_ready - s->out_pos;
        if (n > (size_t)(frames - done))
            n = (size_t)(frames - done);
        const float *src = s->out + s->out_pos * ch;
        for (size_t i = 0; i < n * ch; i++) {
            double v = (double)src[i] * 2147483648.0;
            out[done * ch + i] = v >= 2147483647.0 ? INT32_MAX : v <= -2147483648.0 ? INT32_MIN : (int32_t)floor(v + 0.5);
        }
        s->out_pos += n;
        done += (long)n;
    }
    return done ? done : s->error;
}

static void vorbis_close(void *state)
{
    struct vb_state *s = state;
    if (!s)
        return;
    free_buffers(s);
    vb_setup_free(&s->setup);
    codec_ogg_reader_free(&s->ogg);
    free(s->out);
    free(s);
}

static int vorbis_probe(const uint8_t *data, size_t len)
{
    const uint8_t *p;
    return codec_ogg_first_packet(data, len, is_vorbis, &p) ? 100 : 0;
}

static const struct codec vorbis_codecs[] = {
    {
        .name = "vorbis",
        .description = "Ogg Vorbis",
        .kind = CODEC_AUDIO,
        .caps = CODEC_DECODE,
        .mime_types = "audio/ogg audio/vorbis application/ogg",
        .extensions = "ogg oga",
        .probe = vorbis_probe,
        .audio_open = vorbis_open,
        .audio_read = vorbis_read,
        .audio_close = vorbis_close,
    },
};

CODEC_MODULE(vorbis) = { CODEC_MODULE_ABI, "vorbis", 1, vorbis_codecs };
