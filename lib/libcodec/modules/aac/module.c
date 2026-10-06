/* The AAC module of libcodec. It provides two codecs, one for each
 * container, and both share the decoder. Each frame of either container
 * decodes to 1024 samples per channel. The module does not trim the encoder
 * delay that MP4 edit lists describe, so a file decodes to all of its
 * frames. */
#include "aac.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

struct aac_state {
    struct aac_stream stream;
    struct aac_decoder *decoder;
    float *pcm;                         /* the decoded block */
    unsigned avail, pos;                /* frames in pcm, and the next frame to return */
    const uint8_t *frame;               /* remaining blocks of the current frame */
    size_t frame_len;
    unsigned blocks;
    struct aac_bits bits;
    int ended, error;
};

static int aac_open(const uint8_t *data, size_t len, struct codec_audio_format *fmt, long *frames, void **state)
{
    struct aac_state *s = calloc(1, sizeof *s);
    if (!s)
        return -ENOMEM;
    int err = aac_stream_open(&s->stream, data, len);
    if (err) {
        free(s);
        return err == -ENOTSUP ? err : -EINVAL;
    }
    s->decoder = aac_decoder_new(&s->stream.cfg);
    s->pcm = malloc(sizeof *s->pcm * AAC_FRAME * s->stream.cfg.channels);
    if (!s->decoder || !s->pcm) {
        aac_decoder_free(s->decoder);
        free(s->pcm);
        aac_stream_close(&s->stream);
        free(s);
        return -ENOMEM;
    }
    fmt->rate = (int)s->stream.cfg.rate;
    fmt->channels = (int)s->stream.cfg.channels;
    fmt->bits = 0;                      /* AAC has no sample size */
    *frames = aac_stream_frames(&s->stream);
    *state = s;
    return 0;
}

/* Decodes the next raw data block into pcm. */
static int next_block(struct aac_state *s)
{
    if (!s->blocks) {
        int rc = aac_stream_next(&s->stream, &s->frame, &s->frame_len, &s->blocks);
        if (rc <= 0)
            return rc;
        aac_bits_init(&s->bits, s->frame, s->frame_len);
    }
    s->blocks--;
    int err = aac_decode_block(s->decoder, &s->bits, 0, s->pcm);
    if (err)
        return err == -ENOTSUP ? err : -EBADMSG;
    s->avail = AAC_FRAME;
    s->pos = 0;
    return 1;
}

static long aac_read(void *state, int32_t *out, long frames)
{
    struct aac_state *s = state;
    unsigned ch = s->stream.cfg.channels;
    long done = 0;
    while (done < frames) {
        if (s->pos == s->avail) {
            if (s->ended)
                break;
            int rc = next_block(s);
            if (rc <= 0) {
                s->ended = 1;
                s->error = rc;
                break;
            }
        }
        unsigned n = s->avail - s->pos;
        if (n > (unsigned long)(frames - done))
            n = (unsigned)(frames - done);
        const float *src = s->pcm + s->pos * ch;
        for (unsigned i = 0; i < n * ch; i++) {
            double v = (double)src[i] * 2147483648.0;
            out[done * ch + i] = v >= 2147483647.0 ? INT32_MAX : v <= -2147483648.0 ? INT32_MIN : (int32_t)floor(v + 0.5);
        }
        s->pos += n;
        done += n;
    }
    return done ? done : s->error;
}

static void aac_close(void *state)
{
    struct aac_state *s = state;
    if (!s)
        return;
    aac_decoder_free(s->decoder);
    aac_stream_close(&s->stream);
    free(s->pcm);
    free(s);
}

static const struct codec aac_codecs[] = {
    {
        .name = "aac",
        .description = "AAC-LC in ADTS",
        .kind = CODEC_AUDIO,
        .caps = CODEC_DECODE,
        .mime_types = "audio/aac audio/aacp",
        .extensions = "aac adts",
        .probe = aac_adts_probe,
        .audio_open = aac_open,
        .audio_read = aac_read,
        .audio_close = aac_close,
    },
    {
        .name = "m4a",
        .description = "AAC-LC in MP4",
        .kind = CODEC_AUDIO,
        .caps = CODEC_DECODE,
        .mime_types = "audio/mp4 audio/x-m4a",
        .extensions = "m4a m4b mp4",
        .probe = aac_mp4_probe,
        .audio_open = aac_open,
        .audio_read = aac_read,
        .audio_close = aac_close,
    },
};

CODEC_MODULE(aac) = { CODEC_MODULE_ABI, "aac", 2, aac_codecs };
