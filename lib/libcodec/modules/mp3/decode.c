/* mp3.so: MPEG-1, MPEG-2 and MPEG-2.5 Audio Layer III (docs/design/codecs.md,
 * MP3). The decoder is minimp3 of Lieff (third_party/minimp3, CC0), which
 * the module compiles without changes, for Layer III alone and without
 * SIMD code, with floating point output. minimp3_ex skips ID3v2 and APE
 * tags and an ID3v1 tag at the end. It reads the Xing or Info tag of the
 * first frame and removes the delay and the padding that the LAME
 * extension of the tag records, so that a stream has its original length.
 * The encoder is in encode.c. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "mp3.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wunused-function"
#define MINIMP3_ONLY_MP3
#define MINIMP3_NO_SIMD
#define MINIMP3_NO_STDIO
#define MINIMP3_FLOAT_OUTPUT
#define MINIMP3_IMPLEMENTATION
#include "../../../../third_party/minimp3/minimp3_ex.h"
#pragma GCC diagnostic pop

#define CHUNK 4096

/* ---- probe ---- */

/* The length of the Layer III frame whose header is at p, or 0 when p
 * contains no valid header. */
static int frame_length(const uint8_t *p)
{
    static const int rates[3] = { 44100, 48000, 32000 };
    static const int kbps[2][15] = {
        { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320 },  /* MPEG-1 */
        { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160 },      /* MPEG-2 and 2.5 */
    };
    if (p[0] != 0xff || (p[1] & 0xe0) != 0xe0)
        return 0;
    int version = p[1] >> 3 & 3, layer = p[1] >> 1 & 3, bitrate = p[2] >> 4, rate = p[2] >> 2 & 3;
    if (version == 1 || layer != 1 || bitrate == 0 || bitrate == 15 || rate == 3)
        return 0;
    int hz = rates[rate] >> (version == 3 ? 0 : version == 2 ? 1 : 2);
    int mpeg1 = version == 3;
    return (mpeg1 ? 144000 : 72000) * kbps[!mpeg1][bitrate] / hz + (p[2] >> 1 & 1);
}

/* An ID3v2 tag rates 90. Without one, a frame header at the start rates
 * 80 when the next frame header follows at the end of the frame, and 60
 * when the next header lies beyond the probed bytes. */
static int mp3_probe(const uint8_t *data, size_t len)
{
    if (len >= 10 && memcmp(data, "ID3", 3) == 0 && data[3] < 0xff && data[4] < 0xff &&
        !((data[6] | data[7] | data[8] | data[9]) & 0x80))
        return 90;
    if (len < 4)
        return 0;
    int n = frame_length(data);
    if (n == 0)
        return 0;
    if ((size_t)n + 4 > len)
        return 60;
    return frame_length(data + n) ? 80 : 0;
}

/* ---- decoder ---- */

struct mp3_state {
    mp3dec_ex_t dec;
    int channels;
    float buf[CHUNK];
};

static int mp3_open(const uint8_t *data, size_t len, struct codec_audio_format *fmt, long *frames, void **state)
{
    struct mp3_state *s = calloc(1, sizeof *s);
    if (!s)
        return -ENOMEM;
    int r = mp3dec_ex_open_buf(&s->dec, data, len, MP3D_SEEK_TO_SAMPLE);
    if (r == MP3D_E_MEMORY) {
        free(s);
        return -ENOMEM;
    }
    if (r != 0 || s->dec.info.hz <= 0 || s->dec.info.channels < 1 || s->dec.info.channels > 2) {
        mp3dec_ex_close(&s->dec);
        free(s);
        return -EINVAL;
    }
    s->channels = s->dec.info.channels;
    fmt->rate = s->dec.info.hz;
    fmt->channels = s->channels;
    fmt->bits = 0;                      /* MP3 has no sample size */
    *frames = (long)(s->dec.samples / (uint64_t)s->channels);
    *state = s;
    return 0;
}

static long mp3_read(void *state, int32_t *samples, long frames)
{
    struct mp3_state *s = state;
    size_t want = (size_t)frames * (size_t)s->channels, done = 0;
    while (done < want) {
        size_t step = want - done < CHUNK ? want - done : CHUNK;
        step -= step % (size_t)s->channels;
        if (step == 0)
            break;
        size_t got = mp3dec_ex_read(&s->dec, s->buf, step);
        for (size_t i = 0; i < got; i++) {
            float v = s->buf[i] * 2147483648.0f;
            samples[done + i] = v >= 2147483647.0f ? INT32_MAX : v <= -2147483648.0f ? INT32_MIN : (int32_t)v;
        }
        done += got;
        if (got < step)
            break;
    }
    return (long)(done / (size_t)s->channels);
}

static void mp3_close(void *state)
{
    struct mp3_state *s = state;
    mp3dec_ex_close(&s->dec);
    free(s);
}

static const struct codec mp3_codecs[] = {
    {
        .name = "mp3",
        .description = "MPEG Audio Layer III",
        .kind = CODEC_AUDIO,
        .caps = CODEC_DECODE | CODEC_ENCODE,
        .mime_types = "audio/mpeg audio/mp3",
        .extensions = "mp3",
        .probe = mp3_probe,
        .audio_open = mp3_open,
        .audio_read = mp3_read,
        .audio_close = mp3_close,
        .audio_encode = mp3_encode,
        .audio_encode_options = mp3_encode_options,
    },
};

CODEC_MODULE(mp3) = { CODEC_MODULE_ABI, "mp3", 1, mp3_codecs };
