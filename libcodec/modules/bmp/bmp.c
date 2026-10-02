/* bmp.so: Windows bitmaps. The decoder reads the BITMAPINFOHEADER and its
 * V2 to V5 extensions with 24 bit pixels, or 32 bit pixels in BI_RGB
 * (the fourth byte unused, so opaque) or BI_BITFIELDS with any masks of
 * up to eight bits, bottom up or top down. The encoder writes a
 * BITMAPV4HEADER with 32 bit BI_BITFIELDS pixels and an alpha mask,
 * bottom up, so that alpha survives the file. */
#include <codec/codec.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define FILE_HEADER 14
#define V4_HEADER 108
#define BI_RGB 0
#define BI_BITFIELDS 3
#define MAX_SIDE 16384

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static unsigned le16(const uint8_t *p)
{
    return (unsigned)(p[0] | p[1] << 8);
}

/* The header sizes of BITMAPINFOHEADER, its V2 and V3 extensions, and
 * BITMAPV4HEADER and BITMAPV5HEADER. */
static int known_header(uint32_t size)
{
    return size == 40 || size == 52 || size == 56 || size == 108 || size == 124;
}

static int bmp_probe(const uint8_t *data, size_t len)
{
    if (len < FILE_HEADER + 4 || data[0] != 'B' || data[1] != 'M')
        return 0;
    return known_header(le32(data + FILE_HEADER)) ? 90 : 0;
}

/* A channel mask: the shift of its lowest bit and its width. */
struct field {
    uint32_t mask;
    int shift, bits;
};

static int field_init(struct field *f, uint32_t mask)
{
    f->mask = mask;
    f->shift = f->bits = 0;
    if (!mask)
        return 0;
    while (!(mask & 1)) {
        mask >>= 1;
        f->shift++;
    }
    while (mask & 1) {
        mask >>= 1;
        f->bits++;
    }
    return mask || f->bits > 8 ? -1 : 0;    /* holes, or wider than 8 bits */
}

/* The channel scaled to 8 bits; missing is the value of an absent mask. */
static uint32_t field_get(const struct field *f, uint32_t v, uint32_t missing)
{
    if (!f->mask)
        return missing;
    uint32_t x = (v & f->mask) >> f->shift, max = (1u << f->bits) - 1;
    return (x * 255 + max / 2) / max;
}

static int bmp_decode(const uint8_t *data, size_t len, const struct codec_image_request *req,
                      struct codec_picture *out)
{
    if (!bmp_probe(data, len) || len < FILE_HEADER + 40)
        return -EINVAL;
    const uint8_t *h = data + FILE_HEADER;
    uint32_t hsize = le32(h), offset = le32(data + 10);
    int32_t w = (int32_t)le32(h + 4), hh = (int32_t)le32(h + 8);
    unsigned planes = le16(h + 12), bpp = le16(h + 14);
    uint32_t compression = le32(h + 16);
    int top_down = hh < 0;
    int64_t height = top_down ? -(int64_t)hh : hh;
    if (planes != 1 || w <= 0 || w > MAX_SIDE || height <= 0 || height > MAX_SIDE || (bpp != 24 && bpp != 32) ||
        FILE_HEADER + (size_t)hsize > len)
        return -EINVAL;
    struct field r, g, b, a;
    if (compression == BI_BITFIELDS && bpp == 32) {
        /* The masks are in the header from V2 on, else right after it. */
        const uint8_t *m = h + 40;
        if (FILE_HEADER + 40 + 12 > len)
            return -EINVAL;
        uint32_t amask = hsize >= 56 || FILE_HEADER + 40 + 16 <= offset ? le32(m + 12) : 0;
        if (field_init(&r, le32(m)) < 0 || field_init(&g, le32(m + 4)) < 0 || field_init(&b, le32(m + 8)) < 0 ||
            field_init(&a, amask) < 0)
            return -EINVAL;
    } else if (compression == BI_RGB) {
        field_init(&r, 0x00ff0000);
        field_init(&g, 0x0000ff00);
        field_init(&b, 0x000000ff);
        field_init(&a, 0);
    } else {
        return -EINVAL;
    }
    size_t stride = ((size_t)w * bpp / 8 + 3) & ~(size_t)3;
    if (offset < FILE_HEADER + hsize || offset > len || stride * (size_t)height > len - offset)
        return -EINVAL;
    uint32_t *px = malloc((size_t)w * (size_t)height * 4);
    if (!px)
        return -ENOMEM;
    for (int64_t y = 0; y < height; y++) {
        const uint8_t *row = data + offset + (size_t)(top_down ? y : height - 1 - y) * stride;
        uint32_t *dst = px + (size_t)y * (size_t)w;
        for (int32_t x = 0; x < w; x++) {
            uint32_t v = bpp == 24 ? (uint32_t)row[x * 3] | (uint32_t)row[x * 3 + 1] << 8 | (uint32_t)row[x * 3 + 2] << 16
                                   : le32(row + (size_t)x * 4);
            dst[x] = field_get(&a, v, 255) << 24 | field_get(&r, v, 0) << 16 | field_get(&g, v, 0) << 8 |
                     field_get(&b, v, 0);
        }
    }
    out->w = w;
    out->h = (int)height;
    out->pixels = px;
    return 0;
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

static long bmp_encode(const struct codec_picture *pic, uint8_t **result)
{
    size_t body = (size_t)pic->w * pic->h * 4, total = FILE_HEADER + V4_HEADER + body;
    if (total > 0x7fffffff)
        return -EFBIG;
    uint8_t *f = calloc(1, total);
    if (!f)
        return -ENOMEM;
    f[0] = 'B';
    f[1] = 'M';
    put32(f + 2, (uint32_t)total);
    put32(f + 10, FILE_HEADER + V4_HEADER);
    uint8_t *h = f + FILE_HEADER;
    put32(h, V4_HEADER);
    put32(h + 4, (uint32_t)pic->w);
    put32(h + 8, (uint32_t)pic->h);             /* positive: bottom up */
    put16(h + 12, 1);
    put16(h + 14, 32);
    put32(h + 16, BI_BITFIELDS);
    put32(h + 20, (uint32_t)body);
    put32(h + 24, 2835);                        /* 72 dots per inch */
    put32(h + 28, 2835);
    put32(h + 40, 0x00ff0000);
    put32(h + 44, 0x0000ff00);
    put32(h + 48, 0x000000ff);
    put32(h + 52, 0xff000000);
    memcpy(h + 56, "BGRs", 4);                  /* LCS_sRGB, stored little endian */
    uint8_t *p = h + V4_HEADER;
    for (int y = pic->h - 1; y >= 0; y--)
        for (int x = 0; x < pic->w; x++, p += 4)
            put32(p, pic->pixels[(size_t)y * pic->w + x]);
    *result = f;
    return (long)total;
}

static const struct codec bmp_codecs[] = {
    {
        .name = "bmp",
        .description = "Windows bitmap",
        .kind = CODEC_IMAGE,
        .caps = CODEC_DECODE | CODEC_ENCODE,
        .mime_types = "image/bmp image/x-bmp",
        .extensions = "bmp dib",
        .probe = bmp_probe,
        .image_decode = bmp_decode,
        .image_encode = bmp_encode,
    },
};

CODEC_MODULE(bmp) = { CODEC_MODULE_ABI, "bmp", 1, bmp_codecs };
