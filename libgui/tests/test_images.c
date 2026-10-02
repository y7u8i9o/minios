/* PNG decoding against images written by tools/genicons/genicons.py,
 * whose pixels follow formulas. */
#include <gui/image.h>
#include <gui/paint.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"

static struct image *load(const char *name)
{
    char path[128];
    snprintf(path, sizeof path, "tests/data/%s.png", name);
    struct image *img = image_load(path);
    CHECK(img != NULL, "load %s", path);
    if (img)
        CHECK(img->w == 37 && img->h == 23, "%s size %dx%d", name, img->w, img->h);
    return img;
}

static uint32_t px(const struct image *img, int x, int y)
{
    return img->pixels[(size_t)y * img->w + x];
}

void run_image_tests(void)
{
    struct image *img = load("rgba");
    if (img) {
        int ok = 1;
        for (int y = 0; y < img->h; y++)
            for (int x = 0; x < img->w; x++) {
                uint32_t want = (uint32_t)((255 - x * 2) & 255) << 24 | (uint32_t)((x * 7) & 255) << 16 |
                                (uint32_t)((y * 11) & 255) << 8 | (uint32_t)(((x + y) * 3) & 255);
                if (px(img, x, y) != want)
                    ok = 0;
            }
        CHECK(ok, "rgba pixels match the formula through all five filters");
        image_free(img);
    }
    img = load("rgb");
    if (img) {
        CHECK(px(img, 0, 0) == 0x00000000, "rgb: the tRNS colour is transparent: %08x", px(img, 0, 0));
        CHECK(px(img, 3, 2) == (0xff000000u | 21 << 16 | 22 << 8 | 15), "rgb pixel %08x", px(img, 3, 2));
        image_free(img);
    }
    img = load("grey");
    if (img) {
        CHECK(px(img, 4, 5) == (0xff000000u | 35 << 16 | 35 << 8 | 35), "grey pixel %08x", px(img, 4, 5));
        image_free(img);
    }
    img = load("greya");
    if (img) {
        CHECK(px(img, 4, 5) == ((uint32_t)36 << 24 | 35 << 16 | 35 << 8 | 35), "grey alpha pixel %08x", px(img, 4, 5));
        image_free(img);
    }
    img = load("pal");
    if (img) {
        CHECK(px(img, 0, 0) == 0x00000000, "palette entry 0 transparent: %08x", px(img, 0, 0));
        CHECK(px(img, 2, 0) == ((uint32_t)128 << 24 | 6 << 16 | 10 << 8 | 14), "palette entry 2 half transparent: %08x", px(img, 2, 0));
        CHECK(px(img, 5, 0) == (0xff000000u | 15 << 16 | 25 << 8 | 35), "palette entry 5: %08x", px(img, 5, 0));
        image_free(img);
    }
    img = load("stored");
    if (img) {
        CHECK(px(img, 4, 5) == (0xff000000u | 35 << 16 | 35 << 8 | 35), "stored block pixel %08x", px(img, 4, 5));
        image_free(img);
    }
    /* Bit depths below and above 8, and Adam7 interlacing. */
    img = load("grey4");
    if (img) {
        CHECK(px(img, 5, 3) == (0xff000000u | 136 << 16 | 136 << 8 | 136), "4 bit grey pixel %08x", px(img, 5, 3));
        CHECK(px(img, 36, 22) == (0xff000000u | 170 << 16 | 170 << 8 | 170), "4 bit grey last pixel %08x", px(img, 36, 22));
        image_free(img);
    }
    img = load("pal2");
    if (img) {
        int ok = 1;
        for (int y = 0; y < img->h; y++)
            for (int x = 0; x < img->w; x++) {
                int i = (x * 3 + y) % 4;
                if (px(img, x, y) != (0xff000000u | (uint32_t)(10 + 30 * i) << 16 | (uint32_t)(20 + 30 * i) << 8 | (uint32_t)(30 + 30 * i)))
                    ok = 0;
            }
        CHECK(ok, "2 bit palette pixels match the formula");
        image_free(img);
    }
    img = load("rgba16");
    if (img) {
        int ok = 1;
        for (int y = 0; y < img->h; y++)
            for (int x = 0; x < img->w; x++) {
                uint32_t r = (uint32_t)(x * 1000 * 255 + 32767) / 65535, g = (uint32_t)(y * 2000 * 255 + 32767) / 65535;
                uint32_t b = (uint32_t)((x + y) * 300 * 255 + 32767) / 65535, a = (uint32_t)((65535 - x * 500) * 255 + 32767) / 65535;
                if (px(img, x, y) != (a << 24 | r << 16 | g << 8 | b))
                    ok = 0;
            }
        CHECK(ok, "16 bit RGBA pixels are rounded to 8 bits");
        image_free(img);
    }
    img = load("adam7");
    if (img) {
        int ok = 1;
        for (int y = 0; y < img->h; y++)
            for (int x = 0; x < img->w; x++) {
                uint32_t want = (uint32_t)((255 - x * 2) & 255) << 24 | (uint32_t)((x * 7) & 255) << 16 |
                                (uint32_t)((y * 11) & 255) << 8 | (uint32_t)(((x + y) * 3) & 255);
                if (px(img, x, y) != want)
                    ok = 0;
            }
        CHECK(ok, "interlaced RGBA pixels match the formula in all seven passes");
        image_free(img);
    }
    img = load("grey1i");
    if (img) {
        int ok = 1;
        for (int y = 0; y < img->h; y++)
            for (int x = 0; x < img->w; x++)
                if (px(img, x, y) != ((x ^ y) & 1 ? 0xffffffffu : 0xff000000u))
                    ok = 0;
        CHECK(ok, "interlaced 1 bit grey pixels form the checkerboard");
        image_free(img);
    }
    /* Encoding: an opaque image is written as RGB, a translucent one as
     * RGBA, and both decode to the same pixels. */
    for (int translucent = 0; translucent < 2; translucent++) {
        struct image *src = image_create(301, 97);
        CHECK(src != NULL, "image_create");
        if (!src)
            break;
        for (int y = 0; y < src->h; y++)
            for (int x = 0; x < src->w; x++) {
                uint32_t a = translucent ? (uint32_t)((x * 3 + y) & 255) : 255;
                uint32_t c = x < 150 ? (uint32_t)(x / 10 * 17) << 16 | (uint32_t)(y / 8 * 21) << 8 | 0x40
                                     : (uint32_t)(x * 2654435761u >> 8 ^ (uint32_t)y * 40503u) & 0xffffff;
                src->pixels[(size_t)y * src->w + x] = a << 24 | c;
            }
        uint8_t *data = NULL;
        long n = image_encode_png(src, &data);
        CHECK(n > 0, "image_encode_png returns %ld", n);
        if (n > 0) {
            CHECK(data[25] == (translucent ? 6 : 2), "colour type %d", data[25]);
            struct image *back = image_decode(data, (size_t)n);
            CHECK(back && back->w == 301 && back->h == 97, "encoded image decodes");
            if (back)
                CHECK(memcmp(back->pixels, src->pixels, (size_t)301 * 97 * 4) == 0, "the decoded pixels equal the encoded pixels");
            image_free(back);
            free(data);
        }
        image_free(src);
    }
    /* A large uniform image compresses to a small file. */
    struct image *flat = image_create(1024, 768);
    if (flat) {
        for (size_t i = 0; i < (size_t)1024 * 768; i++)
            flat->pixels[i] = 0xff306080;
        uint8_t *data = NULL;
        long n = image_encode_png(flat, &data);
        CHECK(n > 0 && n < 40000, "uniform 1024x768 image encodes to %ld bytes", n);
        free(data);
        image_free(flat);
    }
    CHECK(image_save_png(NULL, "/nonexistent/x.png") == -EINVAL, "image_save_png rejects a missing image");
    /* Reduction averages areas; enlargement repeats pixels. */
    struct image *two = image_create(2, 2);
    if (two) {
        two->pixels[0] = 0xff000000;
        two->pixels[1] = 0xffffffff;
        two->pixels[2] = 0xffffffff;
        two->pixels[3] = 0xff000000;
        struct image *one = image_scale(two, 1, 1);
        CHECK(one && one->pixels[0] == 0xff808080, "2x2 to 1x1 averages: %08x", one ? one->pixels[0] : 0);
        image_free(one);
        struct image *four = image_scale(two, 4, 4);
        CHECK(four && four->pixels[0] == 0xff000000 && four->pixels[1] == 0xff000000 && four->pixels[2] == 0xffffffff,
              "2x2 to 4x4 repeats pixels");
        image_free(four);
        /* The painter draws it 3 times larger, clipped to its surface. */
        struct surface s = { calloc(8 * 8, 4), 8, 8, 8 };
        struct theme t;
        theme_init_default(&t);
        struct painter p;
        painter_init(&p, &s, &t);
        painter_fill(&p, 0, 0, 8, 8, 0x00ff00ff);
        painter_image_scaled(&p, 2, 2, 6, 6, two);
        CHECK(s.pixels[0] == 0x00ff00ff && s.pixels[2 * 8 + 2] == 0x00000000 && s.pixels[2 * 8 + 5] == 0x00ffffff &&
              s.pixels[5 * 8 + 2] == 0x00ffffff && s.pixels[7 * 8 + 7] == 0x00000000 && s.pixels[1 * 8 + 5] == 0x00ff00ff,
              "painter_image_scaled places and scales the image");
        free(s.pixels);
        image_free(two);
    }
    /* Corrupt data is rejected. */
    uint8_t junk[64] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    CHECK(image_decode(junk, sizeof junk) == NULL, "junk rejected");
    CHECK(image_load("tests/data/missing.png") == NULL, "missing file rejected");
    /* Icons decode and paint with alpha over a background. */
    img = image_load("../user/share/icons/save.png");
    CHECK(img && img->w == 16 && img->h == 16, "icon loads");
    if (img) {
        struct surface s = { calloc(32 * 32, 4), 32, 32, 32 };
        struct theme t;
        theme_init_default(&t);
        struct painter p;
        painter_init(&p, &s, &t);
        painter_fill(&p, 0, 0, 32, 32, 0x00ff00ff);
        painter_image(&p, 8, 8, img);
        CHECK(s.pixels[0] == 0x00ff00ff, "background outside the icon");
        CHECK(s.pixels[8 * 32 + 8] == 0x00ff00ff, "transparent icon corner keeps the background");
        CHECK(s.pixels[9 * 32 + 9] == 0x00202020, "opaque icon pixel painted: %08x", s.pixels[9 * 32 + 9]);
        free(s.pixels);
        image_free(img);
    }
}
