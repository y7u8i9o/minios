/* Audio streams through the registry. A decoder keeps the codec, its
 * state and, when it was opened from a file, the contents of the file. */
#include <codec/codec.h>
#include <errno.h>
#include <stdlib.h>

struct codec_audio {
    const struct codec *codec;
    void *state;
    struct codec_audio_format fmt;
    long frames;
    uint8_t *owned;                 /* the file's contents, or NULL */
};

int codec_audio_open(const struct codec *c, const uint8_t *data, size_t len, const char *path,
                     struct codec_audio **out)
{
    if (!c)
        c = codec_identify(CODEC_AUDIO, data, len, path, CODEC_DECODE);
    if (!c || c->kind != CODEC_AUDIO || !c->audio_open || !c->audio_read)
        return -ENOTSUP;
    struct codec_audio *a = calloc(1, sizeof *a);
    if (!a)
        return -ENOMEM;
    a->codec = c;
    a->frames = -1;
    int err = c->audio_open(data, len, &a->fmt, &a->frames, &a->state);
    if (err < 0) {
        free(a);
        return err;
    }
    *out = a;
    return 0;
}

int codec_audio_open_file(const char *path, struct codec_audio **out)
{
    uint8_t *data;
    size_t len;
    int err = codec_read_file(path, &data, &len);
    if (err < 0)
        return err;
    err = codec_audio_open(NULL, data, len, path, out);
    if (err < 0)
        free(data);
    else
        (*out)->owned = data;
    return err;
}

const struct codec_audio_format *codec_audio_format(const struct codec_audio *a) { return &a->fmt; }
long codec_audio_frames(const struct codec_audio *a) { return a->frames; }
const struct codec *codec_audio_codec(const struct codec_audio *a) { return a->codec; }

long codec_audio_read(struct codec_audio *a, int32_t *samples, long frames)
{
    if (frames <= 0)
        return 0;
    return a->codec->audio_read(a->state, samples, frames);
}

void codec_audio_close(struct codec_audio *a)
{
    if (!a)
        return;
    if (a->codec->audio_close)
        a->codec->audio_close(a->state);
    free(a->owned);
    free(a);
}

long codec_audio_encode(const struct codec *c, const struct codec_audio_format *fmt, const int32_t *samples,
                        long frames, uint8_t **data)
{
    if (!c || c->kind != CODEC_AUDIO || !c->audio_encode)
        return -ENOTSUP;
    if (!fmt || fmt->rate <= 0 || fmt->channels <= 0 || frames < 0 || (frames && !samples))
        return -EINVAL;
    return c->audio_encode(fmt, samples, frames, data);
}

int codec_audio_save(const char *path, const char *name, const struct codec_audio_format *fmt,
                     const int32_t *samples, long frames)
{
    const struct codec *c = name ? codec_find(name) : codec_for_path(CODEC_AUDIO, path, CODEC_ENCODE);
    uint8_t *data;
    long n = codec_audio_encode(c, fmt, samples, frames, &data);
    if (n < 0)
        return (int)n;
    int err = codec_write_file(path, data, (size_t)n);
    free(data);
    return err;
}
