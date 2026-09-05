/* The SVG icon renderer: coverage of straight and curved paths, the
 * fill rules, colours, relative commands, arcs, a file in Font
 * Awesome's layout, and painting at the output scale. */
#include "check.h"
#include <gui/image.h>
#include <gui/paint.h>
#include <stdlib.h>
#include <string.h>

static unsigned alpha(const struct image *i, int x, int y) { return i->pixels[y * i->w + x] >> 24; }
static uint32_t rgb(const struct image *i, int x, int y) { return i->pixels[y * i->w + x] & 0xffffff; }

static struct image *render(const char *svg, int px)
{
    return image_render_svg(svg, strlen(svg), px, 0x2a2a2a);
}

void run_svg_tests(void)
{
    /* A rectangle from 80 to 432 of 512 at 16 px covers columns 2.5 to
     * 13.5: full inside, empty outside, half on the boundary pixels. */
    struct image *i = render("<svg viewBox=\"0 0 512 512\"><path d=\"M80 80H432V432H80Z\"/></svg>", 16);
    CHECK(i != NULL, "rectangle renders");
    if (i) {
        CHECK(i->w == 16 && i->h == 16 && i->scale == 1, "size %dx%d scale %d", i->w, i->h, i->scale);
        CHECK(alpha(i, 8, 8) == 255, "inside opaque: %u", alpha(i, 8, 8));
        CHECK(rgb(i, 8, 8) == 0x2a2a2a, "default colour %06x", rgb(i, 8, 8));
        CHECK(alpha(i, 0, 0) == 0 && alpha(i, 15, 15) == 0, "corners transparent");
        CHECK(alpha(i, 1, 8) == 0, "column before the edge empty: %u", alpha(i, 1, 8));
        CHECK(alpha(i, 2, 8) > 100 && alpha(i, 2, 8) < 160, "boundary column half covered: %u", alpha(i, 2, 8));
        CHECK(alpha(i, 8, 13) > 100 && alpha(i, 8, 13) < 160, "boundary row half covered: %u", alpha(i, 8, 13));
        image_free(i);
    }
    /* Relative commands with a nested square: non zero fills the hole,
     * even odd leaves it. */
    const char *nested = "<svg viewBox=\"0 0 100 100\"><path %s d=\"m10 10h80v80h-80z m20 20h40v40h-40z\"/></svg>";
    char doc[256];
    snprintf(doc, sizeof doc, nested, "");
    i = render(doc, 20);
    CHECK(i && alpha(i, 10, 10) == 255, "non zero winding fills the inner square");
    image_free(i);
    snprintf(doc, sizeof doc, nested, "fill-rule=\"evenodd\"");
    i = render(doc, 20);
    CHECK(i && alpha(i, 10, 10) == 0 && alpha(i, 3, 10) == 255, "even odd leaves the hole: %u %u", alpha(i, 10, 10),
          alpha(i, 3, 10));
    image_free(i);
    /* Curves: a circle from cubic segments, then from two arcs. */
    i = render("<svg viewBox=\"0 0 512 512\"><path d=\"M256 0c141 0 256 115 256 256s-115 256-256 256S0 397 0 256 115 0 256 0z\"/></svg>", 32);
    CHECK(i && alpha(i, 16, 16) == 255 && alpha(i, 1, 1) == 0 && alpha(i, 16, 2) == 255, "cubic circle: %u %u %u",
          alpha(i, 16, 16), alpha(i, 1, 1), alpha(i, 16, 2));
    image_free(i);
    i = render("<svg viewBox=\"0 0 512 512\"><path d=\"M0 256a256 256 0 1 0 512 0a256 256 0 1 0-512 0z\"/></svg>", 32);
    CHECK(i && alpha(i, 16, 16) == 255 && alpha(i, 1, 1) == 0 && alpha(i, 2, 16) == 255, "arc circle: %u %u %u",
          alpha(i, 16, 16), alpha(i, 1, 1), alpha(i, 2, 16));
    image_free(i);
    /* Colour and opacity attributes, and a path without fill. */
    i = render("<svg viewBox=\"0 0 10 10\"><path fill=\"#08f\" fill-opacity=\"0.5\" d=\"M0 0H10V10H0Z\"/>"
               "<path fill=\"none\" d=\"M0 0H10V10H0Z\"/></svg>", 10);
    CHECK(i && rgb(i, 5, 5) == 0x0088ff && alpha(i, 5, 5) > 120 && alpha(i, 5, 5) < 136, "colour %06x alpha %u",
          i ? rgb(i, 5, 5) : 0, i ? alpha(i, 5, 5) : 0);
    image_free(i);
    /* A file in Font Awesome's layout: comment, namespace, a 448 wide
     * view box centred in the square, explicit red fill. */
    i = image_load_svg("tests/data/shape.svg", 16, 0x2a2a2a);
    CHECK(i != NULL, "shape.svg loads");
    if (i) {
        CHECK(rgb(i, 8, 8) == 0xff0000 && alpha(i, 8, 8) == 255, "red centre %06x %u", rgb(i, 8, 8), alpha(i, 8, 8));
        CHECK(alpha(i, 0, 8) == 0 && alpha(i, 15, 8) == 0, "the 448 wide box leaves the outer columns empty");
        CHECK(alpha(i, 2, 8) == 255, "column inside the centred box");
        image_free(i);
    }
    /* Malformed input is refused. */
    CHECK(render("<svg><path d=\"M0 0\"/></svg>", 16) == NULL, "no view box refused");
    CHECK(render("<svg viewBox=\"0 0 10 10\"><path d=\"M0 0 Q\"/></svg>", 16) == NULL, "short command refused");
    /* An image at scale 2 paints one to one on a scale 2 painter and
     * covers 16 logical pixels. */
    i = render("<svg viewBox=\"0 0 512 512\"><path d=\"M0 0H512V512H0Z\"/></svg>", 32);
    if (i) {
        i->scale = 2;
        CHECK(image_lw(i) == 16 && image_lh(i) == 16, "logical size %dx%d", image_lw(i), image_lh(i));
        struct surface s = { calloc(64 * 64, 4), 64, 64, 64 };
        struct theme t;
        theme_init_default(&t);
        struct painter p;
        painter_init_scaled(&p, &s, &t, 2);
        painter_fill(&p, 0, 0, 32, 32, 0x00ff00ff);
        painter_image(&p, 4, 4, i);
        CHECK(s.pixels[8 * 64 + 8] == 0x002a2a2a, "device pixel at the icon's origin: %08x", s.pixels[8 * 64 + 8]);
        CHECK(s.pixels[39 * 64 + 39] == 0x002a2a2a, "last device pixel of the icon: %08x", s.pixels[39 * 64 + 39]);
        CHECK(s.pixels[40 * 64 + 40] == 0x00ff00ff, "pixel after the icon keeps the background: %08x", s.pixels[40 * 64 + 40]);
        free(s.pixels);
        image_free(i);
    }
}
