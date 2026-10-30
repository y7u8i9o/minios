/* The cursor set of /usr/share/cursors (docs/design/cursors.md).
 *
 * The file index of the directory lists each cursor with its hotspot in
 * units of the 20 by 20 view box, and the aliases. A shape is an entry of
 * the index. cursor_image_of renders the SVG file of a shape at 20
 * logical pixels times the screen scale, one view box unit per logical
 * pixel, adds a soft shadow, and caches the image until the scale
 * changes.
 *
 * Without the index or without the file of a shape, X12 shows the arrow
 * of the earlier versions, 12 by 18 logical pixels. Shape 0 is "default"
 * in both cases.
 *
 * The state below belongs to the single thread of X12. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gui/image.h>
#include "comp.h"

#define CURSOR_DIR "/usr/share/cursors"
#define CURSOR_PX 20            /* the size of a cursor in logical pixels and of its view box */
#define MAX_SHAPES 64
#define SHADOW_ALPHA 77         /* the peak darkness of the shadow, out of 255 */

struct shape {
    char name[24];
    int target;                 /* the entry of an alias, else the entry itself */
    float hot_x, hot_y;         /* in view box units */
    struct cursor_image img;
    uint32_t *pixels;           /* owned by the entry of the file */
    int scale;                  /* the scale of pixels, 0 when not rendered */
};

static struct shape shapes[MAX_SHAPES];
static int nshapes;

/* ---- the index ---- */

static int add_shape(const char *name)
{
    if (nshapes == MAX_SHAPES)
        return -1;
    struct shape *s = &shapes[nshapes];
    memset(s, 0, sizeof *s);
    strlcpy(s->name, name, sizeof s->name);
    s->target = nshapes;
    return nshapes++;
}

int cursor_lookup(const char *name)
{
    for (int i = 0; i < nshapes; i++)
        if (strcmp(shapes[i].name, name) == 0)
            return shapes[i].target;
    return -1;
}

const char *cursor_name(int shape)
{
    return shape >= 0 && shape < nshapes ? shapes[shapes[shape].target].name : "default";
}

void cursor_init(void)
{
    add_shape("default");
    FILE *f = fopen(CURSOR_DIR "/index", "r");
    if (!f) {
        comp_log("no cursor index in " CURSOR_DIR ", the built-in arrow is used");
        return;
    }
    char line[128], name[32], target[32];
    float x, y;
    /* The aliases follow the cursors, so a target is known when an alias
     * refers to it. */
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#')
            continue;
        if (sscanf(line, "%31s = %31s", name, target) == 2) {
            int t = cursor_lookup(target), i = t >= 0 ? add_shape(name) : -1;
            if (i >= 0)
                shapes[i].target = t;
        } else if (sscanf(line, "%31s %f %f", name, &x, &y) == 3) {
            int i = strcmp(name, "default") == 0 ? 0 : add_shape(name);
            if (i >= 0) {
                shapes[i].hot_x = x;
                shapes[i].hot_y = y;
            }
        }
    }
    fclose(f);
    comp_log("%d cursors", nshapes);
}

/* ---- the built-in arrow ---- */

#define ARROW_W 12
#define ARROW_H 18

/* The arrow at the scale S as cells of S by S device pixels: X is the
 * black outline, o the white fill, and the shape shifted down and right
 * by one cell is a shadow of 40 percent. */
static int render_arrow(struct shape *s, int S)
{
    static const char *shape[ARROW_H] = {
        "X...........", "XX..........", "XoX.........", "XooX........", "XoooX.......",
        "XooooX......", "XoooooX.....", "XooooooX....", "XoooooooX...", "XooooooooX..",
        "XoooooXXXXX.", "XooXooX.....", "XoX.XooX....", "XX..XooX....", "X....XooX...",
        ".....XooX...", "......XX....", "............",
    };
    int W = (ARROW_W + 1) * S, H = (ARROW_H + 1) * S;
    uint32_t *img = malloc((size_t)W * H * 4);
    if (!img)
        return -1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int i = x / S, j = y / S;
            char c = i < ARROW_W && j < ARROW_H ? shape[j][i] : '.';
            uint32_t v = 0;
            if (c == 'X')
                v = 0xff000000u;
            else if (c == 'o')
                v = 0xffffffffu;
            else if (i >= 1 && j >= 1 && shape[j - 1][i - 1] != '.')
                v = 102u << 24;
            img[(size_t)y * W + x] = v;
        }
    s->pixels = img;
    s->img = (struct cursor_image){ img, W, H, 0, 0 };
    return 0;
}

/* ---- the SVG cursors ---- */

/* One pass of a box blur of radius r over the w x h values of a, along
 * rows (step 1) or columns (step w). */
static void box_blur(uint8_t *a, int w, int h, int r, int along_rows)
{
    int n = along_rows ? w : h, lines = along_rows ? h : w;
    uint8_t *line = malloc((size_t)n);
    if (!line)
        return;
    for (int l = 0; l < lines; l++) {
        uint8_t *p = along_rows ? a + (size_t)l * w : a + l;
        int step = along_rows ? 1 : w;
        for (int i = 0; i < n; i++)
            line[i] = p[i * step];
        int sum = 0;
        for (int i = -r; i <= r; i++)
            sum += i >= 0 && i < n ? line[i] : 0;
        for (int i = 0; i < n; i++) {
            p[i * step] = (uint8_t)(sum / (2 * r + 1));
            int out = i - r, in = i + r + 1;
            sum -= out >= 0 ? line[out] : 0;
            sum += in < n ? line[in] : 0;
        }
    }
    free(line);
}

/* The cursor of S * CURSOR_PX device pixels over its shadow: the alpha of
 * the cursor, moved down by S, blurred twice with a radius of S. The image
 * has a border of 3 S pixels on every side for the shadow. */
static int render_svg(struct shape *s, int S)
{
    char path[96];
    snprintf(path, sizeof path, CURSOR_DIR "/%s.svg", s->name);
    struct image *src = image_load_svg(path, CURSOR_PX * S, 0);
    if (!src)
        return -1;
    int pad = 3 * S, W = src->w + 2 * pad, H = src->h + 2 * pad;
    uint32_t *img = calloc((size_t)W * H, 4);
    uint8_t *shadow = calloc((size_t)W * H, 1);
    if (!img || !shadow) {
        free(img);
        free(shadow);
        image_free(src);
        return -1;
    }
    for (int y = 0; y < src->h; y++)
        for (int x = 0; x < src->w; x++)
            shadow[(size_t)(y + pad + S) * W + x + pad] = (uint8_t)(src->pixels[(size_t)y * src->w + x] >> 24);
    for (int pass = 0; pass < 2; pass++) {
        box_blur(shadow, W, H, S, 1);
        box_blur(shadow, W, H, S, 0);
    }
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            uint32_t sa = (uint32_t)shadow[(size_t)y * W + x] * SHADOW_ALPHA / 255;
            int sx = x - pad, sy = y - pad;
            uint32_t c = sx >= 0 && sy >= 0 && sx < src->w && sy < src->h ? src->pixels[(size_t)sy * src->w + sx] : 0;
            uint32_t a = c >> 24;
            /* The cursor over the black shadow, with straight alpha. */
            uint32_t oa = a + sa * (255 - a) / 255;
            uint32_t out = oa << 24;
            if (oa)
                for (int shift = 0; shift < 24; shift += 8)
                    out |= (((c >> shift) & 255) * a / oa) << shift;
            img[(size_t)y * W + x] = out;
        }
    free(shadow);
    image_free(src);
    s->pixels = img;
    s->img = (struct cursor_image){ img, W, H, (int)(s->hot_x * S + 0.5f) + pad, (int)(s->hot_y * S + 0.5f) + pad };
    return 0;
}

const struct cursor_image *cursor_image_of(int shape)
{
    if (shape < 0 || shape >= nshapes)
        shape = 0;
    struct shape *s = &shapes[shapes[shape].target];
    int S = screen_scale;
    /* A shape is rendered once per scale. A shape that fails remains
     * without pixels until the next scale and shows "default". */
    if (s->scale != S) {
        free(s->pixels);
        s->pixels = NULL;
        s->scale = S;
        if (render_svg(s, S) < 0 && !(s == &shapes[0] && render_arrow(s, S) == 0))
            comp_log("cursor %s cannot be rendered", s->name);
    }
    if (s->pixels)
        return &s->img;
    return s == &shapes[0] ? NULL : cursor_image_of(0);
}
