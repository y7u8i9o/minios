/* M20: parse the TrueType and CFF test fonts, check metrics, kerning,
 * outlines and a rasterized glyph. Exits 0 when every check passes. */
#include <stdio.h>
#include <string.h>
#include <font/font.h>

static int failures;

#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("fonttest: FAIL " __VA_ARGS__); printf("\n"); } } while (0)

static void check_font(const char *path, int cff, int upem, int glyphs, int gid_a, int adv_a, int kern_av, int kern_to,
                       int kern_oo, int ax0, int ay0, int ax1, int ay1)
{
    struct ofont *f = font_open(path);
    CHECK(f != NULL, "open %s", path);
    if (!f)
        return;
    CHECK(font_is_cff(f) == cff, "%s cff flag %d", path, font_is_cff(f));
    CHECK(font_units_per_em(f) == upem, "%s upem %d", path, font_units_per_em(f));
    CHECK(font_glyph_count(f) == glyphs, "%s glyph count %d", path, font_glyph_count(f));
    int a = font_glyph_index(f, 'A'), v = font_glyph_index(f, 'V'), t = font_glyph_index(f, 'T'), o = font_glyph_index(f, 'o');
    CHECK(a == gid_a, "%s glyph id of A is %d", path, a);
    CHECK(font_advance(f, a) == adv_a, "%s advance of A is %d", path, font_advance(f, a));
    CHECK(font_kern(f, a, v) == kern_av, "%s kern AV %d", path, font_kern(f, a, v));
    CHECK(font_kern(f, t, o) == kern_to, "%s kern To %d", path, font_kern(f, t, o));
    CHECK(font_kern(f, o, o) == kern_oo, "%s kern oo %d", path, font_kern(f, o, o));
    CHECK(font_glyph_index(f, 0xffff) == 0, "%s missing code point maps to 0", path);
    struct font_outline out;
    CHECK(font_outline(f, a, &out) == 0, "%s outline of A", path);
    CHECK(out.ncontours == 2 && out.npoints > 8, "%s A has %d contours, %d points", path, out.ncontours, out.npoints);
    CHECK(out.xmin == ax0 && out.ymin == ay0 && out.xmax == ax1 && out.ymax == ay1,
          "%s A bounds %d %d %d %d", path, out.xmin, out.ymin, out.xmax, out.ymax);
    font_outline_free(&out);
    /* Rasterize A at 32 pixels: the bitmap fits the scaled bounds, has
     * fully and partially covered pixels, and its coverage sum matches
     * the glyph area within 10 percent (area from the bounds fraction
     * that a capital A typically covers is not known, so the sum is
     * only checked against the bitmap size). */
    const struct font_glyph *g = font_render(f, a, 32);
    CHECK(g != NULL && g->bitmap != NULL, "%s render A", path);
    if (g && g->bitmap) {
        int want_w = ((font_scale(f, ax1, 32) + 63) >> 6) - (font_scale(f, ax0, 32) >> 6);
        int want_h = ((font_scale(f, ay1, 32) + 63) >> 6) - (font_scale(f, ay0, 32) >> 6);
        CHECK(g->width == want_w && g->height == want_h, "%s A bitmap %dx%d, expected %dx%d", path, g->width, g->height, want_w, want_h);
        CHECK(g->top == -((font_scale(f, ay1, 32) + 63) >> 6), "%s A top %d", path, g->top);
        CHECK(g->advance == font_scale(f, adv_a, 32), "%s A advance %d", path, g->advance);
        long sum = 0;
        int full = 0, partial = 0;
        for (int i = 0; i < g->width * g->height; i++) {
            sum += g->bitmap[i];
            if (g->bitmap[i] == 255) full++;
            else if (g->bitmap[i]) partial++;
        }
        CHECK(full > 30 && partial > 50, "%s A coverage: %d full, %d partial", path, full, partial);
        CHECK(sum > 255L * g->width * g->height / 5 && sum < 255L * g->width * g->height * 2 / 3,
              "%s A coverage sum %ld of %d pixels", path, sum, g->width * g->height);
        printf("fonttest: %s A at 32px: %dx%d, %d full, %d partial, sum %ld\n", path, g->width, g->height, full, partial, sum);
        /* The same bitmap comes back from the cache. */
        CHECK(font_render(f, a, 32) == g, "%s cache hit", path);
    }
    struct font_shaped sh[4];
    int32_t width;
    int n = font_shape(f, "AV", 2, 32, sh, 4, &width);
    CHECK(n == 2 && sh[0].x == 0 && sh[1].x == font_scale(f, adv_a, 32) + font_scale(f, kern_av, 32),
          "%s shaping AV: %d glyphs, x %d %d", path, n, sh[0].x, sh[1].x);
    font_close(f);
}

int main(void)
{
    check_font("/etc/fonts/DejaVuSans.ttf", 0, 2048, 6241, 36, 1401, -131, -348, 0, 16, 0, 1384, 1493);
    /* Latin Modern kerns round pairs apart through its class table. */
    check_font("/etc/fonts/lmroman10-regular.otf", 1, 1000, 821, 27, 750, -111, -83, 28, 32, 0, 717, 716);
    CHECK(font_open("/etc/fonts/sans18.mfnt") == NULL, "a bitmap font file is rejected");
    CHECK(font_open("/nonexistent.ttf") == NULL, "a missing file is rejected");
    printf("fonttest: %d failures\n", failures);
    return failures ? 1 : 0;
}
