/* K4 of docs/plan/widgets.md: the antialiased shapes of the painter and
 * the scaled metrics of the theme. */
#include <gui/paint.h>
#include <gui/pixel.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"

#define SIZE 64

static uint32_t *new_surface(struct surface *s, uint32_t color)
{
    *s = (struct surface){ malloc(SIZE * SIZE * 4), SIZE, SIZE, SIZE };
    for (int i = 0; i < SIZE * SIZE; i++)
        s->pixels[i] = color;
    return s->pixels;
}

static uint32_t at(const struct surface *s, int x, int y) { return s->pixels[y * s->stride + x] & 0xffffff; }

/* The corners of a filled rounded rectangle at scale 1 and 2 take their
 * coverage from pixel_corner_table. */
static void test_corners(const struct theme *t)
{
    for (int scale = 1; scale <= 2; scale++) {
        struct surface s;
        new_surface(&s, 0xffffff);
        struct painter p;
        painter_init_scaled(&p, &s, t, scale);
        painter_round_rect(&p, 2, 2, 20, 20, 6, 0x000000, PAINTER_NONE);
        int R = 6 * scale, X = 2 * scale, Y = 2 * scale, bad = 0;
        const uint8_t *table = pixel_corner_table(R);
        for (int j = 0; j < R; j++)
            for (int i = 0; i < R; i++) {
                uint32_t want = pixel_blend(0xffffff, 0x000000, table[j * R + i]) & 0xffffff;
                /* The four corners are mirror images. */
                int W = 20 * scale;
                bad += at(&s, X + i, Y + j) != want;
                bad += at(&s, X + W - 1 - i, Y + j) != want;
                bad += at(&s, X + i, Y + W - 1 - j) != want;
                bad += at(&s, X + W - 1 - i, Y + W - 1 - j) != want;
            }
        CHECK(bad == 0, "scale %d: %d corner pixels differ from the table", scale, bad);
        CHECK(at(&s, X + R, Y + R) == 0, "scale %d: the inside is filled", scale);
        free(s.pixels);
    }
}

/* A chevron is symmetric about its axis, and the up chevron mirrors the
 * down chevron. */
static void test_marks(const struct theme *t)
{
    struct surface d, u, c;
    new_surface(&d, 0xffffff);
    new_surface(&u, 0xffffff);
    new_surface(&c, 0xffffff);
    struct painter pd, pu, pc;
    painter_init(&pd, &d, t);
    painter_init(&pu, &u, t);
    painter_init(&pc, &c, t);
    painter_chevron(&pd, 0, 0, 20, PAINTER_DOWN, 0);
    painter_chevron(&pu, 0, 0, 20, PAINTER_UP, 0);
    painter_check(&pc, 0, 0, 20, 0);
    int asym = 0, mirror = 0, inked = 0, outside = 0;
    for (int y = 0; y < 20; y++)
        for (int x = 0; x < 20; x++) {
            asym += at(&d, x, y) != at(&d, 19 - x, y);
            mirror += at(&d, x, y) != at(&u, x, 19 - y);
            inked += at(&c, x, y) != 0xffffff;
        }
    for (int y = 0; y < SIZE; y++)
        for (int x = 0; x < SIZE; x++)
            if (x >= 20 || y >= 20)
                outside += at(&c, x, y) != 0xffffff;
    CHECK(asym == 0, "the down chevron is symmetric: %d pixels differ", asym);
    CHECK(mirror == 0, "the up chevron mirrors the down chevron: %d pixels differ", mirror);
    CHECK(inked > 20 && outside == 0, "the check mark lies in its square: %d pixels, %d outside", inked, outside);
    free(d.pixels);
    free(u.pixels);
    free(c.pixels);
}

/* A stroke one pixel wide along the pixel centres from (2.5, 2.5) to
 * (12.5, 2.5) covers its end pixels and no pixel beyond them. */
static void test_stroke(const struct theme *t)
{
    struct surface s;
    new_surface(&s, 0xffffff);
    struct painter p;
    painter_init(&p, &s, t);
    float xy[4] = { 2.5f, 2.5f, 12.5f, 2.5f };
    painter_stroke(&p, xy, 2, 1, 0);
    CHECK(at(&s, 2, 2) == 0 && at(&s, 12, 2) == 0 && at(&s, 7, 2) == 0, "the end points are covered");
    CHECK(at(&s, 14, 2) == 0xffffff && at(&s, 0, 2) == 0xffffff && at(&s, 7, 4) == 0xffffff,
          "no pixel beyond the ends and the width");
    free(s.pixels);
}

static void test_metrics(void)
{
    struct theme t;
    theme_init_default(&t);
    CHECK(theme_px(&t, TM_RADIUS) == 6 && theme_scale_px(&t, 4) == 4, "the metrics at scale 100");
    t.scale = 150;
    CHECK(theme_px(&t, TM_RADIUS) == 9 && theme_scale_px(&t, 4) == 6 && theme_px(&t, TM_ICON) == 24,
          "the metrics follow ui_scale 150: radius %d", theme_px(&t, TM_RADIUS));
}

void run_paint_tests(void)
{
    struct theme t;
    theme_init_default(&t);
    t.font_path[0] = '\0';
    theme_apply(&t);
    test_corners(&t);
    test_marks(&t);
    test_stroke(&t);
    test_metrics();
    theme_release(&t);
}
