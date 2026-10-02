/* wav.so: RIFF WAVE files with linear PCM samples of 8 (unsigned), 16,
 * 24 and 32 bits (signed, little endian) and one to eight channels, in
 * the PCM format tag or the extensible one with the PCM sub-format. The
 * encoder writes the PCM format tag. Samples are converted to and from
 * the library's signed 32 bit samples by shifting, so a file read and
 * written again at its own sample size is unchanged. */
#include <codec/codec.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define FORMAT_PCM 1
#define FORMAT_EXTENSIBLE 0xfffe
#define MAX_CHANNELS 8

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static int wav_probe(const uint8_t *data, size_t len)
{
    return len >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WAVE", 4) == 0 ? 100 : 0;
}

/* The decoder state: the sample data inside the caller's buffer and the
 * read position in frames. */
struct wav_state {
    const uint8_t *data;
    long frames, pos;
    int channels, bytes;
};

static int wav_open(const uint8_t *file, size_t size, struct codec_audio_format *fmt, long *frames, void **state)
{
    if (!wav_probe(file, size))
        return -EINVAL;
    unsigned format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    const uint8_t *data = NULL;
    size_t data_size = 0;
    size_t at = 12;
    while (at + 8 <= size) {
        size_t len = le32(file + at + 4);
        const uint8_t *body = file + at + 8;
        if (len > size - at - 8)
            len = size - at - 8;            /* a truncated last chunk is cut */
        if (memcmp(file + at, "fmt ", 4) == 0 && len >= 16) {
            format = le16(body);
            channels = le16(body + 2);
            rate = le32(body + 4);
            bits = le16(body + 14);
            /* The extensible format names its sample format in the
             * first two bytes of the sub-format GUID. */
            if (format == FORMAT_EXTENSIBLE)
                format = len >= 40 && le16(body + 16) >= 22 ? le16(body + 24) : 0;
        } else if (memcmp(file + at, "data", 4) == 0) {
            data = body;
            data_size = len;
        }
        at += 8 + len + (len & 1);
    }
    if (format != FORMAT_PCM || !data || rate == 0 || channels < 1 || channels > MAX_CHANNELS ||
        (bits != 8 && bits != 16 && bits != 24 && bits != 32))
        return -EINVAL;
    struct wav_state *s = malloc(sizeof *s);
    if (!s)
        return -ENOMEM;
    s->data = data;
    s->channels = (int)channels;
    s->bytes = (int)bits / 8;
    s->frames = (long)(data_size / (channels * (size_t)s->bytes));
    s->pos = 0;
    fmt->rate = (int)rate;
    fmt->channels = (int)channels;
    fmt->bits = (int)bits;
    *frames = s->frames;
    *state = s;
    return 0;
}

static long wav_read(void *state, int32_t *out, long frames)
{
    struct wav_state *s = state;
    if (frames > s->frames - s->pos)
        frames = s->frames - s->pos;
    long n = frames * s->channels;
    const uint8_t *p = s->data + (size_t)s->pos * s->channels * s->bytes;
    for (long i = 0; i < n; i++, p += s->bytes) {
        uint32_t v;
        switch (s->bytes) {
        case 1: v = (uint32_t)(p[0] ^ 0x80) << 24; break;
        case 2: v = (uint32_t)le16(p) << 16; break;
        case 3: v = (uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24; break;
        default: v = le32(p); break;
        }
        out[i] = (int32_t)v;
    }
    s->pos += frames;
    return frames;
}

static void wav_close(void *state)
{
    free(state);
}

static void put16(uint8_t *p, unsigned v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    put16(p, v & 0xffff);
    put16(p + 2, v >> 16);
}

/* A PCM file of fmt->bits (16 when 0) bits per sample. */
static long wav_encode(const struct codec_audio_format *fmt, const int32_t *samples, long frames, uint8_t **result)
{
    int bits = fmt->bits ? fmt->bits : 16;
    if ((bits != 8 && bits != 16 && bits != 24 && bits != 32) || fmt->channels > MAX_CHANNELS)
        return -EINVAL;
    int bytes = bits / 8;
    size_t data_size = (size_t)frames * fmt->channels * bytes;
    if (data_size > 0xffffffffu - 44)
        return -EFBIG;
    size_t total = 44 + data_size + (data_size & 1);
    uint8_t *f = malloc(total);
    if (!f)
        return -ENOMEM;
    memcpy(f, "RIFF", 4);
    put32(f + 4, (uint32_t)(total - 8));
    memcpy(f + 8, "WAVEfmt ", 8);
    put32(f + 16, 16);
    put16(f + 20, FORMAT_PCM);
    put16(f + 22, (unsigned)fmt->channels);
    put32(f + 24, (uint32_t)fmt->rate);
    put32(f + 28, (uint32_t)fmt->rate * (uint32_t)(fmt->channels * bytes));
    put16(f + 32, (unsigned)(fmt->channels * bytes));
    put16(f + 34, (unsigned)bits);
    memcpy(f + 36, "data", 4);
    put32(f + 40, (uint32_t)data_size);
    uint8_t *p = f + 44;
    long n = frames * fmt->channels;
    for (long i = 0; i < n; i++) {
        uint32_t v = (uint32_t)samples[i];
        switch (bytes) {
        case 1: *p++ = (uint8_t)((v >> 24) ^ 0x80); break;
        case 2: put16(p, v >> 16); p += 2; break;
        case 3: p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 24); p += 3; break;
        default: put32(p, v); p += 4; break;
        }
    }
    if (data_size & 1)
        *p = 0;
    *result = f;
    return (long)total;
}

static const struct codec wav_codecs[] = {
    {
        .name = "wav",
        .description = "RIFF WAVE, linear PCM",
        .kind = CODEC_AUDIO,
        .caps = CODEC_DECODE | CODEC_ENCODE,
        .mime_types = "audio/x-wav audio/wav audio/vnd.wave",
        .extensions = "wav wave",
        .probe = wav_probe,
        .audio_open = wav_open,
        .audio_read = wav_read,
        .audio_close = wav_close,
        .audio_encode = wav_encode,
    },
};

CODEC_MODULE(wav) = { CODEC_MODULE_ABI, "wav", 1, wav_codecs };
