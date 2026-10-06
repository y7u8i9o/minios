/* Audio streams through the registry. A decoder retains the codec, its
 * state and, when it was opened from a file, the contents of the file. */
#include <codec/codec.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

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

long codec_audio_encode_options(const struct codec *c, const struct codec_audio_format *fmt, const int32_t *samples,
                                long frames, const char *options, uint8_t **data)
{
    if (!options || !options[0])
        return codec_audio_encode(c, fmt, samples, frames, data);
    if (!c || c->kind != CODEC_AUDIO || !c->audio_encode)
        return -ENOTSUP;
    if (!c->audio_encode_options)
        return -EINVAL;
    if (!fmt || fmt->rate <= 0 || fmt->channels <= 0 || frames < 0 || (frames && !samples))
        return -EINVAL;
    return c->audio_encode_options(fmt, samples, frames, options, data);
}

int codec_audio_save(const char *path, const char *name, const struct codec_audio_format *fmt,
                     const int32_t *samples, long frames)
{
    return codec_audio_save_options(path, name, fmt, samples, frames, NULL);
}

int codec_audio_save_options(const char *path, const char *name, const struct codec_audio_format *fmt,
                             const int32_t *samples, long frames, const char *options)
{
    const struct codec *c = name ? codec_find(name) : codec_for_path(CODEC_AUDIO, path, CODEC_ENCODE);
    uint8_t *data;
    long n = codec_audio_encode_options(c, fmt, samples, frames, options, &data);
    if (n < 0)
        return (int)n;
    int err = codec_write_file(path, data, (size_t)n);
    free(data);
    return err;
}

/* ---- options ---- */

/* The next option of a list: its name and value as pointers and lengths.
 * Returns the position after it, or NULL at the end. */
static const char *next_option(const char *p, const char **name, size_t *nlen, const char **value, size_t *vlen)
{
    while (*p == ',' || *p == ' ')
        p++;
    if (!*p)
        return NULL;
    size_t n = strcspn(p, ",");
    const char *eq = memchr(p, '=', n);
    *name = p;
    *nlen = eq ? (size_t)(eq - p) : n;
    *value = eq ? eq + 1 : p + n;
    *vlen = eq ? n - *nlen - 1 : 0;
    return p + n;
}

int codec_option(const char *options, const char *name, char *value, size_t size)
{
    const char *p = options, *n, *v;
    size_t nlen, vlen;
    while (p && (p = next_option(p, &n, &nlen, &v, &vlen)))
        if (nlen == strlen(name) && strncmp(n, name, nlen) == 0) {
            if (size) {
                size_t k = vlen < size - 1 ? vlen : size - 1;
                memcpy(value, v, k);
                value[k] = '\0';
            }
            return 1;
        }
    return 0;
}

int codec_options_check(const char *options, const char *known)
{
    const char *p = options, *n, *v;
    size_t nlen, vlen;
    while (p && (p = next_option(p, &n, &nlen, &v, &vlen))) {
        int found = 0;
        for (const char *k = known; *k && !found;) {
            while (*k == ' ')
                k++;
            size_t klen = strcspn(k, " ");
            found = klen == nlen && strncmp(k, n, nlen) == 0;
            k += klen;
        }
        if (!found)
            return -EINVAL;
    }
    return 0;
}

void codec_vorbis_channel_order(int *order, unsigned channels)
{
    static const int maps[9][8] = {
        { 0 }, { 0 }, { 0, 1 }, { 0, 2, 1 }, { 0, 1, 2, 3 }, { 0, 2, 1, 3, 4 }, { 0, 2, 1, 5, 3, 4 },
        { 0, 2, 1, 6, 5, 3, 4 }, { 0, 2, 1, 7, 5, 6, 3, 4 },
    };
    for (unsigned c = 0; c < 8; c++)
        order[c] = channels <= 8 ? maps[channels][c] : (int)c;
}
