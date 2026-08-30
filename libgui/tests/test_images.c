/* PNG decoding against images written by tools/genicons/genicons.py,
 * whose pixels follow formulas. */
#include <gui/image.h>
#include <gui/paint.h>
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
