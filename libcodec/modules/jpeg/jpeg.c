/* jpeg.so: JPEG images through stb_image and stb_image_write of the stb
 * project (third_party/stb, public domain or MIT). The decoder reads
 * baseline and progressive JPEG with Huffman coding in grey or colour.
 * Arithmetic coding is not supported, and the EXIF orientation is not
 * applied. The encoder writes baseline JPEG at quality 90, without alpha,
 * which JPEG cannot store. */
#include <codec/codec.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SIDE 16384
#define QUALITY 90

/* The headers define more functions than the module calls, and they are
 * compiled as they were published, without the warnings of minios. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wcomment"

/* stb_image is compiled for JPEG alone, without stdio, SIMD, floating
 * point images or thread local storage, which a module loaded with dlopen
 * does not need. */
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_SIMD
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_THREAD_LOCALS
#define STBI_NO_FAILURE_STRINGS
#define STBI_MAX_DIMENSIONS MAX_SIDE
#define STBI_ASSERT(x) ((void)0)
#define STB_IMAGE_IMPLEMENTATION
#include "../../../third_party/stb/stb_image.h"

#define STBI_WRITE_NO_STDIO
#define STBIW_ASSERT(x) ((void)0)
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../../third_party/stb/stb_image_write.h"
#pragma GCC diagnostic pop

/* A JPEG file begins with the start of image marker and another marker. */
static int jpeg_probe(const uint8_t *data, size_t len)
{
    return len >= 3 && data[0] == 0xff && data[1] == 0xd8 && data[2] == 0xff ? 90 : 0;
}

static int jpeg_decode(const uint8_t *data, size_t len, const struct codec_image_request *req,
                       struct codec_picture *out)
{
    (void)req;
    if (!jpeg_probe(data, len) || len > 0x7fffffff)
        return -EINVAL;
    int w, h, channels;
    uint8_t *rgba = stbi_load_from_memory(data, (int)len, &w, &h, &channels, 4);
    if (!rgba)
        return -EINVAL;
    uint32_t *px = malloc((size_t)w * (size_t)h * 4);
    if (!px) {
        stbi_image_free(rgba);
        return -ENOMEM;
    }
    for (size_t i = 0; i < (size_t)w * (size_t)h; i++) {
        const uint8_t *p = rgba + i * 4;
        px[i] = (uint32_t)p[3] << 24 | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
    }
    stbi_image_free(rgba);
    out->w = w;
    out->h = h;
    out->pixels = px;
    return 0;
}

/* The output of the encoder grows in a buffer that stb_image_write fills
 * through its callback. */
struct sink {
    uint8_t *data;
    size_t len, cap;
    int failed;
};

static void sink_write(void *context, void *bytes, int n)
{
    struct sink *s = context;
    if (s->failed || n <= 0)
        return;
    if (s->len + (size_t)n > s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 65536;
        while (cap < s->len + (size_t)n)
            cap *= 2;
        uint8_t *grown = realloc(s->data, cap);
        if (!grown) {
            s->failed = 1;
            return;
        }
        s->data = grown;
        s->cap = cap;
    }
    memcpy(s->data + s->len, bytes, (size_t)n);
    s->len += (size_t)n;
}

static long jpeg_encode(const struct codec_picture *pic, uint8_t **result)
{
    if (pic->w <= 0 || pic->h <= 0 || pic->w > MAX_SIDE || pic->h > MAX_SIDE)
        return -EFBIG;
    size_t n = (size_t)pic->w * (size_t)pic->h;
    uint8_t *rgb = malloc(n * 3);
    if (!rgb)
        return -ENOMEM;
    for (size_t i = 0; i < n; i++) {
        uint32_t v = pic->pixels[i];
        rgb[i * 3] = (uint8_t)(v >> 16);
        rgb[i * 3 + 1] = (uint8_t)(v >> 8);
        rgb[i * 3 + 2] = (uint8_t)v;
    }
    struct sink s = { 0 };
    int ok = stbi_write_jpg_to_func(sink_write, &s, pic->w, pic->h, 3, rgb, QUALITY);
    free(rgb);
    if (!ok || s.failed) {
        free(s.data);
        return s.failed ? -ENOMEM : -EINVAL;
    }
    *result = s.data;
    return (long)s.len;
}

static const struct codec jpeg_codecs[] = {
    {
        .name = "jpeg",
        .description = "JPEG image",
        .kind = CODEC_IMAGE,
        .caps = CODEC_DECODE | CODEC_ENCODE,
        .mime_types = "image/jpeg image/pjpeg",
        .extensions = "jpg jpeg jpe jfif",
        .probe = jpeg_probe,
        .image_decode = jpeg_decode,
        .image_encode = jpeg_encode,
    },
};

CODEC_MODULE(jpeg) = { CODEC_MODULE_ABI, "jpeg", 1, jpeg_codecs };
