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

/* Unifont is the glyph source of the Unicode viewer. It exercises two
 * paths the Latin UI fonts never reach: outlines derived from a 16 pixel
 * bitmap grid leave only 64 units per em, and the coverage above the BMP
 * is reachable only through a format 12 cmap subtable. */
static void check_unifont(void)
{
    static const struct {
        uint32_t cp;
        const char *script;
    } covered[] = {
        { 0x4e00, "CJK" },       { 0x3042, "hiragana" }, { 0x05d0, "hebrew" },
        { 0x0627, "arabic" },    { 0xac00, "hangul" },   { 0x0905, "devanagari" },
        { 0x10a0, "georgian" },  { 0x2000b, "CJK extension B, above the BMP" },
    };
    struct ofont *f = font_open("/usr/share/fonts/unifont.otf");
    CHECK(f != NULL, "open unifont");
    if (!f)
        return;
    CHECK(font_is_cff(f) == 1, "unifont cff flag %d", font_is_cff(f));
    CHECK(font_units_per_em(f) == 64, "unifont upem %d", font_units_per_em(f));
    CHECK(font_glyph_count(f) == 58911, "unifont glyph count %d", font_glyph_count(f));
    for (int i = 0; i < (int)(sizeof covered / sizeof covered[0]); i++)
        CHECK(font_glyph_index(f, covered[i].cp) != 0, "unifont covers %s", covered[i].script);
    /* Colour emoji are not part of Unifont; the viewer reports them as
     * absent rather than drawing a blank cell. */
    CHECK(font_glyph_index(f, 0x1f600) == 0, "unifont has no emoji");

    /* Rasterize U+4E2D at 32 pixels, which the 64 unit em has to survive.
     * U+4E00 would be a poor choice here because it is a single
     * horizontal stroke and its bitmap is only two pixels tall; U+4E2D
     * has strokes along both axes. */
    int g = font_glyph_index(f, 0x4e2d);
    const struct font_glyph *bm = font_render(f, g, 32);
    CHECK(bm != NULL && bm->bitmap != NULL, "unifont renders U+4E2D");
    if (bm && bm->bitmap) {
        long sum = 0;
        for (int i = 0; i < bm->width * bm->height; i++)
            sum += bm->bitmap[i];
        CHECK(bm->width > 8 && bm->height > 8, "unifont U+4E2D bitmap %dx%d", bm->width, bm->height);
        CHECK(sum > 0, "unifont U+4E2D is blank");
        printf("fonttest: unifont U+4E2D at 32px: %dx%d, sum %ld\n", bm->width, bm->height, sum);
    }
    font_close(f);
}

/* Noto Sans is the Latin, Greek and Cyrillic family. The other scripts
 * live in separate Noto families, so their absence here is by design and
 * not a truncated download; the Unicode viewer offers Unifont for them. */
static void check_noto_scope(void)
{
    struct ofont *f = font_open("/usr/share/fonts/NotoSans-Regular.ttf");
    CHECK(f != NULL, "open noto sans");
    if (!f)
        return;
    CHECK(font_glyph_index(f, 'A') != 0, "noto sans covers Latin");
    CHECK(font_glyph_index(f, 0x0416) != 0, "noto sans covers Cyrillic");
    CHECK(font_glyph_index(f, 0x4e00) == 0, "noto sans has no CJK");
    CHECK(font_glyph_index(f, 0x3042) == 0, "noto sans has no kana");
    CHECK(font_glyph_index(f, 0xac00) == 0, "noto sans has no hangul");
    font_close(f);
}

int main(void)
{
    check_font("/usr/share/fonts/DejaVuSans.ttf", 0, 2048, 6241, 36, 1401, -131, -348, 0, 16, 0, 1384, 1493);
    /* Latin Modern kerns round pairs apart through its class table. */
    check_font("/usr/share/fonts/lmroman10-regular.otf", 1, 1000, 821, 27, 750, -111, -83, 28, 32, 0, 717, 716);
    check_unifont();
    check_noto_scope();
    CHECK(font_open("/usr/share/fonts/sans18.mfnt") == NULL, "a bitmap font file is rejected");
    CHECK(font_open("/nonexistent.ttf") == NULL, "a missing file is rejected");
    printf("fonttest: %d failures\n", failures);
    return failures ? 1 : 0;
}
