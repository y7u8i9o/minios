/* Decoding of Opus audio in Ogg files (RFC 7845) with the reference
 * decoder libopus (RFC 6716) from third_party/opus.
 *
 * The Ogg reader of libcodec returns the packets of the first Opus stream
 * in the file, followed by the packets of every chained Opus stream that
 * has the same channel count. Each stream begins with an identification
 * header (OpusHead) and a comment header (OpusTags). The module decodes
 * every audio packet at 48 kHz, whatever input rate the header records.
 *
 * The module discards the first pre-skip samples of each stream, and the
 * granule position of the last page trims the end of the stream. The end
 * is known only when a page with a granule position arrives, so the module
 * returns decoded samples only up to the last such page. It reorders the
 * channels of mapping family 1 from the Vorbis order to the WAV order. */
#include <codec/codec.h>
#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <opus.h>
#include <opus_multistream.h>

#define OPUS_RATE 48000
#define MAX_PACKET_FRAMES 5760          /* 120 ms at 48 kHz, the longest packet */
#define MAX_CHANNELS 8

/* The identification header (section 5.1 of RFC 7845). */
struct opus_head {
    unsigned channels;
    unsigned preskip;
    uint32_t input_rate;
    int gain;                           /* Q7.8 dB */
    unsigned family;
    unsigned streams, coupled;
    unsigned char mapping[MAX_CHANNELS];
};

struct opus_state {
    struct codec_ogg_reader ogg;
    OpusMSDecoder *decoder;
    unsigned channels;                  /* of the first stream; chained streams must match it */
    int order[MAX_CHANNELS];            /* decoder channel for each WAV position */
    int headers;                        /* header packets read from the current stream */
    unsigned preskip;
    int64_t decoded;                    /* samples decoded from this stream, pre-skip included */
    int64_t kept;                       /* frames of the current stream appended to out so far */
    int granule_seen;                   /* this stream has had a page with a granule position */
    float *pcm;                         /* one decoded packet */
    /* Decoded samples in WAV order. opus_read may return the frames before
     * out_ready to its caller. The frames after it wait for the next
     * granule position, which may trim them. chain_start is the index of the first
     * frame of the current stream in this buffer. */
    float *out;
    size_t out_n, out_cap, out_pos, out_ready, chain_start;
    int ended, error;
};

static int is_opus(const uint8_t *packet, size_t len)
{
    return len >= 19 && memcmp(packet, "OpusHead", 8) == 0;
}

static unsigned le16(const uint8_t *p)
{
    return p[0] | (unsigned)p[1] << 8;
}

static int parse_head(const uint8_t *p, size_t len, struct opus_head *h)
{
    if (!is_opus(p, len) || (p[8] >> 4) != 0)
        return -EINVAL;                 /* only major version 0 is defined */
    h->channels = p[9];
    h->preskip = le16(p + 10);
    h->input_rate = p[12] | (uint32_t)p[13] << 8 | (uint32_t)p[14] << 16 | (uint32_t)p[15] << 24;
    h->gain = (int16_t)le16(p + 16);
    h->family = p[18];
    if (h->channels == 0)
        return -EINVAL;
    if (h->family == 0) {
        if (h->channels > 2)
            return -EINVAL;
        h->streams = 1;
        h->coupled = h->channels - 1;
        h->mapping[0] = 0;
        h->mapping[1] = 1;
        return 0;
    }
    if (h->family != 1 || h->channels > MAX_CHANNELS || len < 21u + h->channels)
        return -EINVAL;
    h->streams = p[19];
    h->coupled = p[20];
    if (h->streams == 0 || h->coupled > h->streams || h->streams + h->coupled > 255)
        return -EINVAL;
    for (unsigned c = 0; c < h->channels; c++) {
        h->mapping[c] = p[21 + c];
        if (h->mapping[c] != 255 && h->mapping[c] >= h->streams + h->coupled)
            return -EINVAL;
    }
    return 0;
}

/* Start decoding a new stream with the header h. */
static int start_stream(struct opus_state *s, const struct opus_head *h)
{
    if (s->decoder)
        opus_multistream_decoder_destroy(s->decoder);
    int err;
    s->decoder = opus_multistream_decoder_create(OPUS_RATE, (int)h->channels, (int)h->streams, (int)h->coupled,
                                                 h->mapping, &err);
    if (!s->decoder)
        return err == OPUS_ALLOC_FAIL ? -ENOMEM : -EBADMSG;
    if (h->gain)
        opus_multistream_decoder_ctl(s->decoder, OPUS_SET_GAIN(h->gain));
    s->preskip = h->preskip;
    s->decoded = 0;
    s->kept = 0;
    s->granule_seen = 0;
    s->chain_start = s->out_n;
    s->out_ready = s->out_n;
    return 0;
}

static int reserve(struct opus_state *s, size_t frames)
{
    if (s->out_pos && s->out_pos == s->out_ready) {
        /* The buffer no longer needs the frames returned so far. */
        size_t keep = s->out_n - s->out_pos;
        memmove(s->out, s->out + s->out_pos * s->channels, keep * s->channels * sizeof *s->out);
        s->out_n = keep;
        s->chain_start = s->chain_start > s->out_pos ? s->chain_start - s->out_pos : 0;
        s->out_ready -= s->out_pos;
        s->out_pos = 0;
    }
    if (s->out_n + frames <= s->out_cap)
        return 0;
    size_t cap = s->out_cap ? s->out_cap : 4 * MAX_PACKET_FRAMES;
    while (cap < s->out_n + frames)
        cap *= 2;
    float *grown = realloc(s->out, cap * s->channels * sizeof *s->out);
    if (!grown)
        return -ENOMEM;
    s->out = grown;
    s->out_cap = cap;
    return 0;
}

/* Decode one audio packet and append its samples to the output, except for
 * the part that the pre-skip covers. */
static int audio_packet(struct opus_state *s, const uint8_t *data, size_t len)
{
    int n = opus_multistream_decode_float(s->decoder, data, (opus_int32)len, s->pcm, MAX_PACKET_FRAMES, 0);
    if (n < 0)
        return n == OPUS_ALLOC_FAIL ? -ENOMEM : -EBADMSG;
    int64_t skip = (int64_t)s->preskip - s->decoded;
    unsigned first = skip <= 0 ? 0 : skip >= n ? (unsigned)n : (unsigned)skip;
    s->decoded += n;
    int err = reserve(s, (size_t)n - first);
    if (err)
        return err;
    unsigned ch = s->channels;
    float *dst = s->out + s->out_n * ch;
    for (unsigned i = first; i < (unsigned)n; i++, dst += ch)
        for (unsigned k = 0; k < ch; k++)
            dst[k] = s->pcm[i * ch + (k < MAX_CHANNELS ? (unsigned)s->order[k] : k)];
    s->out_n += (unsigned)n - first;
    s->kept += (unsigned)n - first;
    return 0;
}

/* A page of the current stream ended with the granule position granule. It
 * counts the samples of the stream up to the end of the page, pre-skip
 * included (section 4 of RFC 7845). Two cases require removing frames that
 * have not yet been returned to the caller:
 *
 * - On the first page with a granule position, a value below the number
 *   of decoded samples means that the stream starts later than its
 *   packets do. The function removes the surplus from the start.
 * - On the last page of the stream, the encoder may have padded the final
 *   packet. The function removes the frames beyond the granule position
 *   from the end. */
static void apply_granule(struct opus_state *s, int64_t granule, int eos)
{
    if (!s->granule_seen && granule < s->decoded && !eos) {
        /* No frame of the stream is returned before its first granule
         * position, so all of its frames lie after chain_start. */
        size_t frames = s->out_n - s->chain_start;
        int64_t drop = s->decoded - granule;
        size_t d = drop > (int64_t)frames ? frames : (size_t)drop;
        float *at = s->out + s->chain_start * s->channels;
        memmove(at, at + d * s->channels, (frames - d) * s->channels * sizeof *s->out);
        s->out_n -= d;
        s->kept -= (int64_t)d;
    } else if (eos) {
        int64_t keep = granule - (int64_t)s->preskip;
        int64_t excess = s->kept - (keep > 0 ? keep : 0);
        size_t pending = s->out_n - s->out_ready;
        if (excess > 0) {
            size_t d = excess > (int64_t)pending ? pending : (size_t)excess;
            s->out_n -= d;
            s->kept -= (int64_t)d;
        }
    }
    s->granule_seen = 1;
    s->out_ready = s->out_n;
}

/* Read packets until new frames are ready, the file ends or an error
 * occurs. */
static int advance(struct opus_state *s)
{
    while (s->out_ready == s->out_pos && !s->ended && !s->error) {
        struct codec_ogg_packet p;
        int rc = codec_ogg_next(&s->ogg, &p);
        if (rc <= 0) {
            if (rc < 0)
                s->error = rc;
            s->ended = 1;
            s->out_ready = s->out_n;    /* the file ends without a final granule position */
            break;
        }
        if (p.bos) {
            struct opus_head h;
            if (parse_head(p.data, p.len, &h) < 0) {
                s->error = -EBADMSG;
                break;
            }
            if (h.channels != s->channels) {
                s->ended = 1;           /* a chained stream with a different channel count */
                s->out_ready = s->out_n;
                break;
            }
            s->out_ready = s->out_n;
            int err = start_stream(s, &h);
            if (err) {
                s->error = err;
                break;
            }
            s->headers = 1;
            continue;
        }
        if (s->headers == 1) {
            if (p.len < 8 || memcmp(p.data, "OpusTags", 8) != 0) {
                s->error = -EBADMSG;
                break;
            }
            s->headers = 2;
            continue;
        }
        int err = audio_packet(s, p.data, p.len);
        if (err) {
            s->error = err;
            break;
        }
        if (p.granule >= 0)
            apply_granule(s, p.granule, p.eos);
    }
    return s->error;
}

/* The number of frames in the file. For each chained Opus stream, this is
 * the granule position of its last page minus its pre-skip. The function
 * returns -1 when a stream has no page with a granule position. */
static long count_frames(const uint8_t *data, size_t len, unsigned channels)
{
    struct codec_ogg_reader r;
    codec_ogg_reader_init(&r, data, len, is_opus);
    struct codec_ogg_packet p;
    long total = 0;
    int64_t last = -1;
    unsigned preskip = 0;
    int rc;
    while ((rc = codec_ogg_next(&r, &p)) > 0) {
        if (p.bos) {
            struct opus_head h;
            if (parse_head(p.data, p.len, &h) < 0 || h.channels != channels)
                break;
            if (last >= 0)
                total += last > preskip ? (long)(last - preskip) : 0;
            preskip = h.preskip;
            last = -1;
        } else if (p.granule >= 0) {
            last = p.granule;
        }
    }
    codec_ogg_reader_free(&r);
    if (rc < 0 || last < 0)
        return -1;
    return total + (last > preskip ? (long)(last - preskip) : 0);
}

static int opus_open(const uint8_t *data, size_t len, struct codec_audio_format *fmt, long *frames, void **state)
{
    const uint8_t *id;
    size_t idlen = codec_ogg_first_packet(data, len, is_opus, &id);
    struct opus_head h;
    if (!idlen || parse_head(id, idlen, &h) < 0)
        return -EINVAL;
    struct opus_state *s = calloc(1, sizeof *s);
    if (!s)
        return -ENOMEM;
    s->channels = h.channels;
    s->pcm = malloc(MAX_PACKET_FRAMES * h.channels * sizeof *s->pcm);
    if (!s->pcm) {
        free(s);
        return -ENOMEM;
    }
    if (h.family == 1)
        codec_vorbis_channel_order(s->order, h.channels);
    else
        for (unsigned c = 0; c < MAX_CHANNELS; c++)
            s->order[c] = (int)c;
    codec_ogg_reader_init(&s->ogg, data, len, is_opus);
    fmt->rate = OPUS_RATE;
    fmt->channels = (int)h.channels;
    fmt->bits = 0;                      /* Opus has no sample size */
    *frames = count_frames(data, len, h.channels);
    *state = s;
    return 0;
}

static long opus_read(void *state, int32_t *out, long frames)
{
    struct opus_state *s = state;
    long done = 0;
    unsigned ch = s->channels;
    while (done < frames) {
        if (s->out_pos == s->out_ready)
            advance(s);
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

static void opus_close(void *state)
{
    struct opus_state *s = state;
    if (!s)
        return;
    if (s->decoder)
        opus_multistream_decoder_destroy(s->decoder);
    codec_ogg_reader_free(&s->ogg);
    free(s->pcm);
    free(s->out);
    free(s);
}

static int opus_probe(const uint8_t *data, size_t len)
{
    return codec_ogg_probe(data, len, is_opus);
}

static const struct codec opus_codecs[] = {
    {
        .name = "opus",
        .description = "Ogg Opus",
        .kind = CODEC_AUDIO,
        .caps = CODEC_DECODE,
        .mime_types = "audio/opus",
        .extensions = "opus",
        .probe = opus_probe,
        .audio_open = opus_open,
        .audio_read = opus_read,
        .audio_close = opus_close,
    },
};

CODEC_MODULE(opus) = { CODEC_MODULE_ABI, "opus", 1, opus_codecs };
