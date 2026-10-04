/* Host test of the rasterizer: narrow glyphs whose left edge lies past
 * the first pixel column must get coverage (the crossing sort once
 * compared shifted and unshifted x, which emptied '.' at 28 px and '!'
 * at 14 px). */
#include <font/font.h>
#include <stdio.h>
#include <stdlib.h>

static int failures;

static int coverage(const struct font_glyph *g)
{
    int n = 0;
    for (int i = 0; g && g->bitmap && i < g->width * g->height; i++)
        n += g->bitmap[i] > 128;
    return n;
}

static void expect_ink(struct ofont *f, int c, int px, int min)
{
    int g = font_glyph_index(f, (uint32_t)c);
    const struct font_glyph *r = font_render(f, g, px);
    int n = coverage(r);
    if (!r || n < min) {
        printf("FAIL '%c' at %d px: %d solid pixels (expected at least %d)\n", c, px, n, min);
        failures++;
    }
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "../../third_party/dejavu/DejaVuSans.ttf";
    struct ofont *f = font_open(path);
    if (!f) {
        printf("cannot open %s\n", path);
        return 1;
    }
    static const int sizes[] = { 12, 14, 20, 28, 40 };
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        expect_ink(f, '.', sizes[i], 1);
        expect_ink(f, '!', sizes[i], sizes[i] / 4);
        expect_ink(f, 'i', sizes[i], sizes[i] / 4);
        expect_ink(f, 'l', sizes[i], sizes[i] / 4);
        expect_ink(f, ':', sizes[i], 2);
        expect_ink(f, 'o', sizes[i], sizes[i]);
    }
    /* Advances scale with the size. */
    int g = font_glyph_index(f, 'M');
    if (font_scale(f, font_advance(f, g), 28) != 2 * font_scale(f, font_advance(f, g), 14) &&
        abs(font_scale(f, font_advance(f, g), 28) - 2 * font_scale(f, font_advance(f, g), 14)) > 1) {
        printf("FAIL advance does not scale\n");
        failures++;
    }
    font_close(f);
    printf("libfont tests: %d failures\n", failures);
    return failures ? 1 : 0;
}
