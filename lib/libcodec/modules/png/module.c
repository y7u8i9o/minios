/* png.so: Portable Network Graphics, decoded at every colour type and
 * bit depth and encoded as 8 bit RGB or RGBA. */
#include "png.h"

const uint8_t png_signature[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };

static int png_probe(const uint8_t *data, size_t len)
{
    return len >= 8 && memcmp(data, png_signature, 8) == 0 ? 100 : 0;
}

static const struct codec png_codecs[] = {
    {
        .name = "png",
        .description = "Portable Network Graphics",
        .kind = CODEC_IMAGE,
        .caps = CODEC_DECODE | CODEC_ENCODE,
        .mime_types = "image/png",
        .extensions = "png",
        .probe = png_probe,
        .image_decode = png_decode,
        .image_encode = png_encode,
    },
};

CODEC_MODULE(png) = { CODEC_MODULE_ABI, "png", 1, png_codecs };
