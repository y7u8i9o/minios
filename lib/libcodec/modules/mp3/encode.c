/* The encoder of mp3.so: shine (third_party/shine, compiled by shine.c).
 *
 * shine encodes 16 bit samples of one or two channels at the rates of
 * MPEG-1 (32, 44.1 and 48 kHz), MPEG-2 (16, 22.05 and 24 kHz) and
 * MPEG-2.5 (8, 11.025 and 12 kHz) at a constant bit rate. The option
 * bitrate selects the rate in kbit/s from the rates of the MPEG version.
 * The default is 128 kbit/s for two channels and 64 kbit/s for one at
 * MPEG-1, and half of that at MPEG-2 and MPEG-2.5.
 *
 * The output starts with a frame that carries an Info tag in the manner
 * of LAME: the number of audio frames, the length of the file and, in the
 * LAME extension, the delay at the start and the padding at the end. The
 * encoder string of the extension is "minios". minimp3 reads the
 * extension of every encoder and returns exactly the samples of the
 * input. FFmpeg reads the extension only for the encoder strings of LAME
 * and FFmpeg. For a file of this encoder FFmpeg therefore also returns
 * the delay and the padding, as silence. A decoder that does not read the
 * tag decodes the frame as silence. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "mp3.h"
#include "../../../../third_party/shine/layer3.h"

/* The samples that shine and the decoder together add before the input:
 * 528 of the analysis filter bank and the MDCT of the encoder, and 529 of
 * the synthesis of the decoder. An impulse through encoder and decoder
 * appears exactly this many samples later. */
#define CODEC_DELAY 1057

/* The bit rate tables of shine (tables.c). */
extern const int bitrates[16][4];

struct buffer {
    uint8_t *data;
    size_t len, cap;
};

static int append(struct buffer *b, const uint8_t *p, size_t n)
{
    if (b->len + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 65536;
        while (cap < b->len + n)
            cap *= 2;
        uint8_t *d = realloc(b->data, cap);
        if (!d)
            return -ENOMEM;
        b->data = d;
        b->cap = cap;
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return 0;
}

static int16_t to16(int32_t v)
{
    int64_t r = ((int64_t)v + 0x8000) >> 16;
    return (int16_t)(r > 32767 ? 32767 : r);
}

/* The length of the frame at p from its header, for the rates and the bit
 * rates of shine. */
static size_t frame_length(const uint8_t *p, int version, int rate)
{
    int kbps = bitrates[p[2] >> 4][version], padding = p[2] >> 1 & 1;
    return (size_t)((version == MPEG_I ? 144000 : 72000) * kbps / rate + padding);
}

/* shine_flush writes the last frame without the bytes after its data. The
 * frames are walked by their headers, and zero bytes complete the last
 * frame to the length that its header gives. A decoder treats bytes after
 * the data of a frame as ancillary data. */
static int complete_last_frame(struct buffer *audio, int version, int rate)
{
    size_t at = 0, end = 0;
    while (at + 4 <= audio->len && audio->data[at] == 0xff) {
        end = at + frame_length(audio->data + at, version, rate);
        at = end;
    }
    if (end <= audio->len)
        return 0;
    size_t missing = end - audio->len;
    uint8_t zero[64] = { 0 };
    while (missing > 0) {
        size_t n = missing < sizeof zero ? missing : sizeof zero;
        int err = append(audio, zero, n);
        if (err < 0)
            return err;
        missing -= n;
    }
    return 0;
}

/* The frame with the Info tag. Its header has the version, the rate and
 * the mode of the stream, and the lowest bit rate whose frame has room
 * for the tag. Its side information is zero, which decodes as silence. */
static int info_frame(struct buffer *out, int version, int rate_index, int mono, long frames, long bytes,
                      int delay, int padding)
{
    static const int rates[9] = { 44100, 48000, 32000, 22050, 24000, 16000, 11025, 12000, 8000 };
    int mpeg1 = version == MPEG_I, side = mpeg1 ? (mono ? 17 : 32) : (mono ? 9 : 17);
    int need = 4 + side + 8 + 4 + 4 + 36, index = 1, size = 0;
    for (; index < 15; index++) {
        int kbps = bitrates[index][version];
        if (kbps <= 0)
            continue;
        size = (mpeg1 ? 144000 : 72000) * kbps / rates[rate_index];
        if (size >= need)
            break;
    }
    if (index == 15)
        return -EINVAL;
    uint8_t *f = calloc(1, (size_t)size);
    if (!f)
        return -ENOMEM;
    f[0] = 0xff;
    f[1] = (uint8_t)(0xe0 | version << 3 | 1 << 1 | 1);   /* Layer III, no CRC */
    f[2] = (uint8_t)(index << 4 | (rate_index % 3) << 2);
    f[3] = (uint8_t)((mono ? 3 : 0) << 6 | 1 << 2);        /* the mode, original */
    uint8_t *t = f + 4 + side;
    memcpy(t, "Info", 4);
    t[7] = 0x03;                                            /* frames and bytes */
    long total = bytes + size;
    for (int i = 0; i < 4; i++) {
        t[8 + i] = (uint8_t)(frames >> (24 - 8 * i));
        t[12 + i] = (uint8_t)(total >> (24 - 8 * i));
    }
    /* The LAME extension: the encoder string, then at offset 21 the delay
     * and the padding in 12 bits each. */
    uint8_t *x = t + 16;
    memcpy(x, "minios", 6);
    x[21] = (uint8_t)(delay >> 4);
    x[22] = (uint8_t)((delay & 15) << 4 | padding >> 8);
    x[23] = (uint8_t)padding;
    int err = append(out, f, (size_t)size);
    free(f);
    return err;
}

long mp3_encode_options(const struct codec_audio_format *fmt, const int32_t *samples, long frames,
                        const char *options, uint8_t **data)
{
    char value[32];
    if (codec_options_check(options, "bitrate") < 0)
        return -EINVAL;
    int channels = fmt->channels, rate_index = shine_find_samplerate_index(fmt->rate);
    if (channels < 1 || channels > 2 || rate_index < 0)
        return -EINVAL;
    int version = shine_mpeg_version(rate_index);
    int kbps = (version == MPEG_I ? 128 : 64) / (channels == 1 ? 2 : 1);
    if (codec_option(options, "bitrate", value, sizeof value)) {
        char *end;
        kbps = (int)strtol(value, &end, 10);
        if (*end || end == value)
            return -EINVAL;
    }
    if (shine_check_config(fmt->rate, kbps) < 0)
        return -EINVAL;
    shine_config_t config;
    shine_set_config_mpeg_defaults(&config.mpeg);
    config.wave.channels = channels == 1 ? PCM_MONO : PCM_STEREO;
    config.wave.samplerate = fmt->rate;
    config.mpeg.mode = channels == 1 ? MONO : STEREO;
    config.mpeg.bitr = kbps;
    shine_t s = shine_initialise(&config);
    if (!s)
        return -ENOMEM;
    long per_frame = shine_samples_per_pass(s);
    /* Enough frames for the input and the delay, so that the end of the
     * input reaches the output. */
    long nframes = (frames + CODEC_DELAY + per_frame - 1) / per_frame;
    struct buffer audio = { 0 };
    int16_t pcm[SHINE_MAX_SAMPLES * 2];
    int err = 0;
    for (long f = 0; f < nframes && err == 0; f++) {
        for (long i = 0; i < per_frame; i++) {
            long at = f * per_frame + i;
            for (int c = 0; c < channels; c++)
                pcm[i * channels + c] = at < frames ? to16(samples[at * channels + c]) : 0;
        }
        int written = 0;
        unsigned char *bytes = shine_encode_buffer_interleaved(s, pcm, &written);
        if (written > 0)
            err = append(&audio, bytes, (size_t)written);
    }
    if (err == 0) {
        int written = 0;
        unsigned char *bytes = shine_flush(s, &written);
        if (written > 0)
            err = append(&audio, bytes, (size_t)written);
    }
    shine_close(s);
    if (err == 0)
        err = complete_last_frame(&audio, version, fmt->rate);
    struct buffer out = { 0 };
    long padding = nframes * per_frame - CODEC_DELAY - frames;
    if (err == 0)
        err = info_frame(&out, version, rate_index, channels == 1, nframes, (long)audio.len,
                         CODEC_DELAY - MP3_DECODER_DELAY, (int)padding + MP3_DECODER_DELAY);
    if (err == 0)
        err = append(&out, audio.data, audio.len);
    free(audio.data);
    if (err < 0) {
        free(out.data);
        return err;
    }
    *data = out.data;
    return (long)out.len;
}

long mp3_encode(const struct codec_audio_format *fmt, const int32_t *samples, long frames, uint8_t **data)
{
    return mp3_encode_options(fmt, samples, frames, NULL, data);
}
