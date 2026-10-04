/* Images through the registry: the codec is chosen by the content of the
 * data, then by the extension of the file name, and for saving by name
 * or by the extension of the target. */
#include <codec/codec.h>
#include <errno.h>
#include <stdlib.h>

int codec_image_decode(const struct codec *c, const uint8_t *data, size_t len, const char *path,
                       const struct codec_image_request *req, struct codec_picture *out)
{
    static const struct codec_image_request none = { 0, 0, 0 };
    if (!c)
        c = codec_identify(CODEC_IMAGE, data, len, path, CODEC_DECODE);
    if (!c || c->kind != CODEC_IMAGE || !c->image_decode)
        return -ENOTSUP;
    out->w = out->h = 0;
    out->pixels = NULL;
    return c->image_decode(data, len, req ? req : &none, out);
}

int codec_image_load(const char *path, const struct codec_image_request *req, struct codec_picture *out)
{
    uint8_t *data;
    size_t len;
    int err = codec_read_file(path, &data, &len);
    if (err < 0)
        return err;
    err = codec_image_decode(NULL, data, len, path, req, out);
    free(data);
    return err;
}

long codec_image_encode(const struct codec *c, const struct codec_picture *pic, uint8_t **data)
{
    if (!c || c->kind != CODEC_IMAGE || !c->image_encode)
        return -ENOTSUP;
    if (!pic || pic->w <= 0 || pic->h <= 0 || !pic->pixels)
        return -EINVAL;
    return c->image_encode(pic, data);
}

int codec_image_save(const struct codec_picture *pic, const char *path, const char *name)
{
    const struct codec *c = name ? codec_find(name) : codec_for_path(CODEC_IMAGE, path, CODEC_ENCODE);
    uint8_t *data;
    long n = codec_image_encode(c, pic, &data);
    if (n < 0)
        return (int)n;
    int err = codec_write_file(path, data, (size_t)n);
    free(data);
    return err;
}

void codec_picture_free(struct codec_picture *pic)
{
    if (pic) {
        free(pic->pixels);
        pic->pixels = NULL;
        pic->w = pic->h = 0;
    }
}
