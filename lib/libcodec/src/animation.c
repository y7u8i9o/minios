/* Animations through the registry. A decoder retains the codec, its
 * state, the data for a rewind and, when it was opened from a file, the
 * contents of the file. A rewind closes the state of the codec and opens
 * a new one on the same data. */
#include <codec/codec.h>
#include <errno.h>
#include <stdlib.h>

struct codec_animation {
    const struct codec *codec;
    void *state;
    struct codec_animation_info info;
    const uint8_t *data;
    size_t len;
    uint8_t *owned;                 /* the file's contents, or NULL */
};

static int animated(const struct codec *c)
{
    return c && c->kind == CODEC_IMAGE && (c->caps & CODEC_ANIMATED) && c->animation_open && c->animation_next;
}

int codec_animation_open(const struct codec *c, const uint8_t *data, size_t len, const char *path,
                         struct codec_animation **out)
{
    if (!c)
        c = codec_identify(CODEC_IMAGE, data, len, path, CODEC_DECODE | CODEC_ANIMATED);
    if (!animated(c))
        return -ENOTSUP;
    struct codec_animation *a = calloc(1, sizeof *a);
    if (!a)
        return -ENOMEM;
    a->codec = c;
    a->data = data;
    a->len = len;
    int err = c->animation_open(data, len, &a->info, &a->state);
    if (err < 0) {
        free(a);
        return err;
    }
    *out = a;
    return 0;
}

int codec_animation_open_file(const char *path, struct codec_animation **out)
{
    uint8_t *data;
    size_t len;
    int err = codec_read_file(path, &data, &len);
    if (err < 0)
        return err;
    err = codec_animation_open(NULL, data, len, path, out);
    if (err < 0)
        free(data);
    else
        (*out)->owned = data;
    return err;
}

const struct codec_animation_info *codec_animation_info(const struct codec_animation *a) { return &a->info; }
const struct codec *codec_animation_codec(const struct codec_animation *a) { return a->codec; }

int codec_animation_next(struct codec_animation *a, uint32_t *pixels, int *delay_ms)
{
    if (!a->state)
        return -EINVAL;
    return a->codec->animation_next(a->state, pixels, delay_ms);
}

int codec_animation_rewind(struct codec_animation *a)
{
    if (a->state && a->codec->animation_close)
        a->codec->animation_close(a->state);
    a->state = NULL;
    struct codec_animation_info info;
    int err = a->codec->animation_open(a->data, a->len, &info, &a->state);
    if (err < 0)
        a->state = NULL;
    return err;
}

void codec_animation_close(struct codec_animation *a)
{
    if (!a)
        return;
    if (a->state && a->codec->animation_close)
        a->codec->animation_close(a->state);
    free(a->owned);
    free(a);
}

long codec_animation_encode(const struct codec *c, const struct codec_animation_info *info,
                            const struct codec_frame *frames, int count, uint8_t **data)
{
    if (!c || c->kind != CODEC_IMAGE || !(c->caps & CODEC_ANIMATED) || !c->animation_encode)
        return -ENOTSUP;
    if (!info || info->w <= 0 || info->h <= 0 || info->loops < 0 || count <= 0 || !frames)
        return -EINVAL;
    for (int i = 0; i < count; i++)
        if (!frames[i].pixels || frames[i].delay_ms < 0)
            return -EINVAL;
    return c->animation_encode(info, frames, count, data);
}

int codec_animation_save(const char *path, const char *name, const struct codec_animation_info *info,
                         const struct codec_frame *frames, int count)
{
    const struct codec *c = name ? codec_find(name)
                                 : codec_for_path(CODEC_IMAGE, path, CODEC_ENCODE | CODEC_ANIMATED);
    uint8_t *data;
    long n = codec_animation_encode(c, info, frames, count, &data);
    if (n < 0)
        return (int)n;
    int err = codec_write_file(path, data, (size_t)n);
    free(data);
    return err;
}
