/* Rasterizer: outlines scaled to 26.6 pixels, curves flattened, edges
 * scan converted with non zero winding into 8 bit coverage using four
 * sub scanlines per row and exact horizontal coverage. Glyph bitmaps
 * are stored in a per font cache. */
#include "internal.h"
#include <stdlib.h>
#include <errno.h>

#define SUBROWS 4

struct edge {
    int32_t x0, y0, x1, y1;     /* 26.6, y0 < y1 */
    int dir;                    /* +1 downwards in bitmap space, -1 upwards */
};

struct edges {
    struct edge *e;
    int n, cap;
    int32_t xmin, xmax, ymin, ymax;
};

int32_t font_scale(const struct ofont *f, int units, int px)
{
    return (int32_t)(((int64_t)units * px * 64 + (units >= 0 ? f->upem / 2 : -f->upem / 2)) / f->upem);
}

static int add_edge(struct edges *es, int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    if (y0 == y1)
        return 0;
    if (es->n == es->cap) {
        int cap = es->cap ? es->cap * 2 : 256;
        struct edge *e = realloc(es->e, (size_t)cap * sizeof *e);
        if (!e)
            return -ENOMEM;
        es->e = e;
        es->cap = cap;
    }
    struct edge *e = &es->e[es->n++];
    if (y0 < y1) {
        *e = (struct edge){ x0, y0, x1, y1, 1 };
    } else {
        *e = (struct edge){ x1, y1, x0, y0, -1 };
    }
    if (es->n == 1) {
        es->xmin = x0 < x1 ? x0 : x1;
        es->xmax = x0 > x1 ? x0 : x1;
        es->ymin = e->y0;
        es->ymax = e->y1;
    } else {
        if (x0 < es->xmin) es->xmin = x0;
        if (x1 < es->xmin) es->xmin = x1;
        if (x0 > es->xmax) es->xmax = x0;
        if (x1 > es->xmax) es->xmax = x1;
        if (e->y0 < es->ymin) es->ymin = e->y0;
        if (e->y1 > es->ymax) es->ymax = e->y1;
    }
    return 0;
}

static int segments_for(int32_t dx, int32_t dy)
{
    int32_t d = (dx < 0 ? -dx : dx) + (dy < 0 ? -dy : dy);
    int n = d / 192 + 2;                     /* one segment per three pixels */
    return n > 24 ? 24 : n;
}

static int quad(struct edges *es, int32_t x0, int32_t y0, int32_t cx, int32_t cy, int32_t x1, int32_t y1)
{
    int n = segments_for(x1 - x0, y1 - y0);
    int32_t px = x0, py = y0;
    for (int i = 1; i <= n; i++) {
        int64_t t = (int64_t)i * 65536 / n, u = 65536 - t;
        int32_t x = (int32_t)((u * u * x0 + 2 * u * t * cx + t * t * x1) >> 32);
        int32_t y = (int32_t)((u * u * y0 + 2 * u * t * cy + t * t * y1) >> 32);
        int r = add_edge(es, px, py, x, y);
        if (r < 0)
            return r;
        px = x;
        py = y;
    }
    return 0;
}

static int cubic(struct edges *es, int32_t x0, int32_t y0, int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t x1, int32_t y1)
{
    int n = segments_for(x1 - x0, y1 - y0);
    int32_t px = x0, py = y0;
    for (int i = 1; i <= n; i++) {
        int64_t t = (int64_t)i * 4096 / n, u = 4096 - t;   /* 12 bit parameters confine products to 64 bits */
        int64_t w0 = u * u * u, w1 = 3 * u * u * t, w2 = 3 * u * t * t, w3 = t * t * t;
        int32_t x = (int32_t)((w0 * x0 + w1 * ax + w2 * bx + w3 * x1) >> 36);
        int32_t y = (int32_t)((w0 * y0 + w1 * ay + w2 * by + w3 * y1) >> 36);
        int r = add_edge(es, px, py, x, y);
        if (r < 0)
            return r;
        px = x;
        py = y;
    }
    return 0;
}

/* Convert one contour (font units) to edges in 26.6 bitmap pixels: x to
 * the right, y downwards. */
static int contour_edges(const struct ofont *f, int px, const struct font_point *pts, int n, struct edges *es)
{
    if (n < 2)
        return 0;
    int32_t *sx = malloc((size_t)n * sizeof *sx), *sy = malloc((size_t)n * sizeof *sy);
    if (!sx || !sy) {
        free(sx);
        free(sy);
        return -ENOMEM;
    }
    for (int i = 0; i < n; i++) {
        sx[i] = font_scale(f, pts[i].x, px);
        sy[i] = -font_scale(f, pts[i].y, px);
    }
    /* Find a starting on-curve point (or synthesize one between two
     * quadratic controls). */
    int start = -1;
    for (int i = 0; i < n; i++)
        if (pts[i].on_curve == PT_ON) {
            start = i;
            break;
        }
    int32_t x0, y0;
    if (start < 0) {
        x0 = (sx[0] + sx[1]) / 2;
        y0 = (sy[0] + sy[1]) / 2;
        start = 0;
    } else {
        x0 = sx[start];
        y0 = sy[start];
    }
    int32_t cx = x0, cy = y0;
    int r = 0;
    int i = start;
    int pending_quad = 0;
    int32_t qx = 0, qy = 0;
    int cubic_n = 0;
    int32_t c[4];
    for (int k = 0; k < n && r >= 0; k++) {
        i = (start + 1 + k) % n;
        int kind = pts[i].on_curve;
        int32_t x = sx[i], y = sy[i];
        if (i == start && k == n - 1 && pts[start].on_curve != PT_ON)
            kind = PT_ON, x = x0, y = y0;
        if (kind == PT_CUBIC) {
            if (cubic_n < 4) {
                c[cubic_n++] = x;
                c[cubic_n++] = y;
            }
            continue;
        }
        if (kind == PT_QUAD) {
            if (pending_quad) {
                int32_t mx = (qx + x) / 2, my = (qy + y) / 2;
                r = quad(es, cx, cy, qx, qy, mx, my);
                cx = mx;
                cy = my;
            }
            qx = x;
            qy = y;
            pending_quad = 1;
            continue;
        }
        if (cubic_n == 4) {
            r = cubic(es, cx, cy, c[0], c[1], c[2], c[3], x, y);
            cubic_n = 0;
        } else if (pending_quad) {
            r = quad(es, cx, cy, qx, qy, x, y);
            pending_quad = 0;
        } else {
            r = add_edge(es, cx, cy, x, y);
        }
        cx = x;
        cy = y;
    }
    if (r >= 0) {
        if (cubic_n == 4)
            r = cubic(es, cx, cy, c[0], c[1], c[2], c[3], x0, y0);
        else if (pending_quad)
            r = quad(es, cx, cy, qx, qy, x0, y0);
        else
            r = add_edge(es, cx, cy, x0, y0);
    }
    free(sx);
    free(sy);
    return r;
}

/* Add coverage (0..64 per sub row, scaled to 0..255 total) for the span
 * [xa, xb) in 26.6 pixels relative to the bitmap origin. */
static void span(int32_t *row, int width, int32_t xa, int32_t xb)
{
    if (xb <= xa)
        return;
    if (xa < 0) xa = 0;
    if (xb > width * 64) xb = width * 64;
    if (xb <= xa)
        return;
    int ca = xa >> 6, cb = (xb - 1) >> 6;
    if (ca == cb) {
        row[ca] += xb - xa;
        return;
    }
    row[ca] += 64 - (xa & 63);
    for (int c = ca + 1; c < cb; c++)
        row[c] += 64;
    row[cb] += ((xb - 1) & 63) + 1;
}

struct crossing {
    int32_t x;
    int dir;
};

static int render_edges(struct edges *es, struct font_glyph *g)
{
    int x0 = es->xmin >> 6, x1 = (es->xmax + 63) >> 6;
    int y0 = es->ymin >> 6, y1 = (es->ymax + 63) >> 6;
    int width = x1 - x0, height = y1 - y0;
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096)
        return -EINVAL;
    g->bitmap = calloc((size_t)width * height, 1);
    int32_t *row = malloc((size_t)width * sizeof *row);
    struct crossing *cr = malloc((size_t)es->n * sizeof *cr);
    if (!g->bitmap || !row || !cr) {
        free(row);
        free(cr);
        return -ENOMEM;
    }
    g->width = width;
    g->height = height;
    g->left = x0;
    g->top = y0;
    int32_t ox = x0 * 64;
    for (int py = 0; py < height; py++) {
        memset(row, 0, (size_t)width * sizeof *row);
        for (int s = 0; s < SUBROWS; s++) {
            int32_t sy = (y0 + py) * 64 + (64 * s + 32) / SUBROWS;
            int nc = 0;
            for (int i = 0; i < es->n; i++) {
                const struct edge *e = &es->e[i];
                if (sy < e->y0 || sy >= e->y1)
                    continue;
                int64_t x = e->x0 + ((int64_t)(e->x1 - e->x0) * (sy - e->y0)) / (e->y1 - e->y0);
                int32_t rx = (int32_t)x - ox;       /* relative to the bitmap, like cr[].x */
                /* Insertion sort by x. */
                int j = nc++;
                while (j > 0 && cr[j - 1].x > rx) {
                    cr[j] = cr[j - 1];
                    j--;
                }
                cr[j].x = rx;
                cr[j].dir = e->dir;
            }
            int wind = 0;
            for (int i = 0; i + 1 < nc; i++) {
                wind += cr[i].dir;
                if (wind != 0)
                    span(row, width, cr[i].x, cr[i + 1].x);
            }
        }
        uint8_t *out = g->bitmap + (size_t)py * width;
        for (int px = 0; px < width; px++) {
            int32_t v = row[px];            /* 0 .. 64 * SUBROWS */
            v = v * 255 / (64 * SUBROWS);
            out[px] = (uint8_t)(v > 255 ? 255 : v);
        }
    }
    free(row);
    free(cr);
    return 0;
}

static int rasterize(const struct ofont *f, int glyph, int px, struct font_glyph *g)
{
    struct font_outline o;
    int r = font_outline(f, glyph, &o);
    if (r < 0)
        return r;
    struct edges es = { 0 };
    int start = 0;
    for (int c = 0; c < o.ncontours && r >= 0; c++) {
        r = contour_edges(f, px, o.points + start, o.contour_end[c] - start + 1, &es);
        start = o.contour_end[c] + 1;
    }
    font_outline_free(&o);
    memset(g, 0, sizeof *g);
    g->advance = font_scale(f, font_advance(f, glyph), px);
    if (r >= 0 && es.n)
        r = render_edges(&es, g);
    free(es.e);
    return r;
}

const struct font_glyph *font_render(struct ofont *f, int glyph, int px)
{
    if (px <= 0 || px > 512)
        return NULL;
    struct cache_entry *oldest = &f->cache[0];
    for (int i = 0; i < FONT_CACHE_SIZE; i++) {
        struct cache_entry *e = &f->cache[i];
        if (e->stamp && e->glyph == glyph && e->px == px) {
            e->stamp = ++f->stamp;
            return &e->g;
        }
        if (e->stamp < oldest->stamp)
            oldest = e;
    }
    free(oldest->g.bitmap);
    memset(&oldest->g, 0, sizeof oldest->g);
    oldest->stamp = 0;
    if (rasterize(f, glyph, px, &oldest->g) < 0)
        return NULL;
    oldest->glyph = glyph;
    oldest->px = px;
    oldest->stamp = ++f->stamp;
    return &oldest->g;
}

void cache_free(struct ofont *f)
{
    for (int i = 0; i < FONT_CACHE_SIZE; i++)
        free(f->cache[i].g.bitmap);
}
