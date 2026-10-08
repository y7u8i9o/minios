#include <gui/gfx.h>
#include <gui/pixel.h>
#include <string.h>

struct rect rect_intersect(struct rect a, struct rect b)
{
    int x0 = a.x > b.x ? a.x : b.x;
    int y0 = a.y > b.y ? a.y : b.y;
    int x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    struct rect r = { x0, y0, x1 - x0, y1 - y0 };
    if (r.w < 0) r.w = 0;
    if (r.h < 0) r.h = 0;
    return r;
}

struct rect rect_union(struct rect a, struct rect b)
{
    if (rect_empty(a)) return b;
    if (rect_empty(b)) return a;
    int x0 = a.x < b.x ? a.x : b.x;
    int y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    struct rect r = { x0, y0, x1 - x0, y1 - y0 };
    return r;
}

int rect_subtract(struct rect a, struct rect b, struct rect out[4])
{
    struct rect i = rect_intersect(a, b);
    if (rect_empty(i)) {
        out[0] = a;
        return 1;
    }
    int n = 0;
    if (i.y > a.y) out[n++] = (struct rect){ a.x, a.y, a.w, i.y - a.y };
    if (i.y + i.h < a.y + a.h) out[n++] = (struct rect){ a.x, i.y + i.h, a.w, a.y + a.h - (i.y + i.h) };
    if (i.x > a.x) out[n++] = (struct rect){ a.x, i.y, i.x - a.x, i.h };
    if (i.x + i.w < a.x + a.w) out[n++] = (struct rect){ i.x + i.w, i.y, a.x + a.w - (i.x + i.w), i.h };
    return n;
}

/* ---- rectangle sets ---- */

void rect_set_clear(struct rect_set *s)
{
    s->n = 0;
}

struct rect rect_set_bounds(const struct rect_set *s)
{
    struct rect b = { 0, 0, 0, 0 };
    for (int i = 0; i < s->n; i++)
        b = rect_union(b, s->r[i]);
    return b;
}

static long area(struct rect r)
{
    return rect_empty(r) ? 0 : (long)r.w * r.h;
}

static void collapse(struct rect_set *s, struct rect extra)
{
    struct rect b = rect_union(rect_set_bounds(s), extra);
    s->n = 1;
    s->r[0] = b;
}

/* The pieces of r that no rectangle of the set covers, at most max of
 * them. Returns their number, or -1 when they do not fit. */
#define PIECES_MAX (4 * RECT_SET_MAX)
static int uncovered(const struct rect_set *s, struct rect r, struct rect *pieces)
{
    int n = 1;
    pieces[0] = r;
    for (int i = 0; i < s->n && n; i++) {
        struct rect next[PIECES_MAX];
        int m = 0;
        for (int k = 0; k < n; k++) {
            struct rect parts[4];
            int c = rect_subtract(pieces[k], s->r[i], parts);
            if (m + c > PIECES_MAX)
                return -1;
            for (int j = 0; j < c; j++)
                next[m++] = parts[j];
        }
        for (int k = 0; k < m; k++)
            pieces[k] = next[k];
        n = m;
    }
    return n;
}

void rect_set_add(struct rect_set *s, struct rect r)
{
    if (rect_empty(r))
        return;
    for (;;) {
        int merged = 0;
        for (int i = 0; i < s->n; i++) {
            struct rect e = s->r[i];
            if (rect_contains(e, r.x, r.y) && rect_contains(e, r.x + r.w - 1, r.y + r.h - 1))
                return;
            struct rect u = rect_union(e, r);
            long covered = area(e) + area(r) - area(rect_intersect(e, r));
            int inside = rect_contains(r, e.x, e.y) && rect_contains(r, e.x + e.w - 1, e.y + e.h - 1);
            if (inside || area(u) * 4 <= covered * 5) {
                /* r takes the place of e, and may now reach others. */
                s->r[i] = s->r[--s->n];
                r = u;
                merged = 1;
                break;
            }
        }
        if (!merged)
            break;
    }
    struct rect pieces[PIECES_MAX];
    int n = uncovered(s, r, pieces);
    if (n < 0 || s->n + n > RECT_SET_MAX) {
        collapse(s, r);
        return;
    }
    for (int k = 0; k < n; k++)
        s->r[s->n++] = pieces[k];
}

/* ---- window resize zones ---- */

/* The four corner squares of f, in the order top left, top right, bottom
 * left, bottom right.  A square reaches z->reach outside the frame and the
 * inset of its row inside. */
static void corner_squares(struct rect f, const struct gui_resize_zones *z, struct rect sq[4])
{
    int r = z->reach, top = r + z->inset_top, bottom = r + z->inset_bottom;
    sq[0] = (struct rect){ f.x - r, f.y - r, top, top };
    sq[1] = (struct rect){ f.x + f.w - z->inset_top, f.y - r, top, top };
    sq[2] = (struct rect){ f.x - r, f.y + f.h - z->inset_bottom, bottom, bottom };
    sq[3] = (struct rect){ f.x + f.w - z->inset_bottom, f.y + f.h - z->inset_bottom, bottom, bottom };
}

int gui_resize_edges(struct rect f, const struct gui_resize_zones *z, int x, int y)
{
    static const int corner_edges[4] = {
        GUI_EDGE_TOP | GUI_EDGE_LEFT, GUI_EDGE_TOP | GUI_EDGE_RIGHT,
        GUI_EDGE_BOTTOM | GUI_EDGE_LEFT, GUI_EDGE_BOTTOM | GUI_EDGE_RIGHT,
    };
    struct rect sq[4];
    corner_squares(f, z, sq);
    for (int i = 0; i < 4; i++)
        if (rect_contains(sq[i], x, y))
            return corner_edges[i];
    struct rect outer = { f.x - z->margin, f.y - z->margin, f.w + 2 * z->margin, f.h + 2 * z->margin };
    struct rect inside = { f.x + z->inner, f.y + z->inner, f.w - 2 * z->inner, f.h - 2 * z->inner };
    if (!rect_contains(outer, x, y) || rect_contains(inside, x, y))
        return 0;
    int e = 0;
    if (x < inside.x) e |= GUI_EDGE_LEFT;
    if (x >= inside.x + inside.w) e |= GUI_EDGE_RIGHT;
    if (y < inside.y) e |= GUI_EDGE_TOP;
    if (y >= inside.y + inside.h) e |= GUI_EDGE_BOTTOM;
    /* The corner zones continue along the edges. */
    if (e & (GUI_EDGE_LEFT | GUI_EDGE_RIGHT)) {
        if (y < f.y + z->corner) e |= GUI_EDGE_TOP;
        if (y >= f.y + f.h - z->corner) e |= GUI_EDGE_BOTTOM;
    }
    if (e & (GUI_EDGE_TOP | GUI_EDGE_BOTTOM)) {
        if (x < f.x + z->corner) e |= GUI_EDGE_LEFT;
        if (x >= f.x + f.w - z->corner) e |= GUI_EDGE_RIGHT;
    }
    return e;
}

int gui_resize_region(struct rect f, const struct gui_resize_zones *z, struct rect out[5])
{
    out[0] = (struct rect){ f.x - z->margin, f.y - z->margin, f.w + 2 * z->margin, f.h + 2 * z->margin };
    corner_squares(f, z, out + 1);
    return 5;
}

static struct rect clip_to(struct surface *s, int x, int y, int w, int h)
{
    struct rect whole = { 0, 0, s->width, s->height };
    struct rect r = { x, y, w, h };
    return rect_intersect(whole, r);
}

void gfx_fill_rect(struct surface *s, int x, int y, int w, int h, uint32_t color)
{
    struct rect r = clip_to(s, x, y, w, h);
    for (int j = 0; j < r.h; j++)
        pixel_fill(s->pixels + (size_t)(r.y + j) * s->stride + r.x, r.w, color);
}

void gfx_disc(struct surface *s, int x, int y, int size, int inset, uint32_t color, const struct rect *clip)
{
    const uint8_t *t = pixel_disc_table(size, inset);
    struct rect r = clip_to(s, x, y, size, size);
    if (clip)
        r = rect_intersect(r, *clip);
    if (!t)
        return;
    for (int j = r.y; j < r.y + r.h; j++) {
        uint32_t *row = s->pixels + (size_t)j * s->stride;
        const uint8_t *cov = t + (size_t)(j - y) * size - x;
        for (int i = r.x; i < r.x + r.w; i++)
            if (cov[i])
                row[i] = pixel_blend(row[i], color, cov[i]);
    }
}

void gfx_fill(struct surface *s, uint32_t color)
{
    gfx_fill_rect(s, 0, 0, s->width, s->height, color);
}

void gfx_hline(struct surface *s, int x, int y, int w, uint32_t color)
{
    gfx_fill_rect(s, x, y, w, 1, color);
}

void gfx_vline(struct surface *s, int x, int y, int h, uint32_t color)
{
    gfx_fill_rect(s, x, y, 1, h, color);
}

void gfx_rect(struct surface *s, int x, int y, int w, int h, uint32_t color)
{
    gfx_hline(s, x, y, w, color);
    gfx_hline(s, x, y + h - 1, w, color);
    gfx_vline(s, x, y, h, color);
    gfx_vline(s, x + w - 1, y, h, color);
}

void gfx_line(struct surface *s, int x0, int y0, int x1, int y1, uint32_t color)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        if (x0 >= 0 && y0 >= 0 && x0 < s->width && y0 < s->height)
            s->pixels[(size_t)y0 * s->stride + x0] = color;
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void gfx_text(struct surface *s, int x, int y, const char *text, uint32_t fg, uint32_t bg)
{
    for (; *text; text++, x += GFX_FONT_W) {
        const uint8_t *glyph = gfx_font8x16[(unsigned char)*text];
        for (int row = 0; row < GFX_FONT_H; row++) {
            int py = y + row;
            if (py < 0 || py >= s->height)
                continue;
            uint8_t bits = glyph[row];
            for (int col = 0; col < GFX_FONT_W; col++) {
                int px = x + col;
                if (px < 0 || px >= s->width)
                    continue;
                if (bits & (0x80 >> col))
                    s->pixels[(size_t)py * s->stride + px] = fg;
                else if (bg != 0xffffffffu)
                    s->pixels[(size_t)py * s->stride + px] = bg;
            }
        }
    }
}

int gfx_text_width(const char *text)
{
    return (int)strlen(text) * GFX_FONT_W;
}

void gfx_blit(struct surface *dst, int dx, int dy, const struct surface *src, const struct rect *clip)
{
    struct rect target = { dx, dy, src->width, src->height };
    struct rect whole = { 0, 0, dst->width, dst->height };
    struct rect r = rect_intersect(target, whole);
    if (clip)
        r = rect_intersect(r, *clip);
    for (int j = 0; j < r.h; j++) {
        const uint32_t *from = src->pixels + (size_t)(r.y - dy + j) * src->stride + (r.x - dx);
        uint32_t *to = dst->pixels + (size_t)(r.y + j) * dst->stride + r.x;
        memcpy(to, from, (size_t)r.w * 4);
    }
}

void gfx_copy_rect(struct surface *dst, const struct surface *src, const struct rect *rr)
{
    struct rect whole = { 0, 0, dst->width, dst->height };
    struct rect r = rect_intersect(*rr, whole);
    for (int j = 0; j < r.h; j++)
        memcpy(dst->pixels + (size_t)(r.y + j) * dst->stride + r.x,
               src->pixels + (size_t)(r.y + j) * src->stride + r.x, (size_t)r.w * 4);
}

/* The rows are copied from the top when the pixels move up and from the
 * bottom when they move down, so that no source row is overwritten before
 * it is copied. memmove handles the overlap within a row. */
void gfx_move_rect(struct surface *s, struct rect r, int dx, int dy)
{
    int w = r.w - (dx < 0 ? -dx : dx), h = r.h - (dy < 0 ? -dy : dy);
    if (w <= 0 || h <= 0)
        return;
    int sx = dx < 0 ? r.x - dx : r.x, tx = dx < 0 ? r.x : r.x + dx;
    int sy = dy < 0 ? r.y - dy : r.y, ty = dy < 0 ? r.y : r.y + dy;
    for (int k = 0; k < h; k++) {
        int j = dy > 0 ? h - 1 - k : k;
        memmove(s->pixels + (size_t)(ty + j) * s->stride + tx, s->pixels + (size_t)(sy + j) * s->stride + sx,
                (size_t)w * 4);
    }
}

/* ---- colours ---- */

/* a / b rounded to the nearest integer, for a negative a as well. */
static int div_nearest(int a, int b)
{
    return a >= 0 ? (a + b / 2) / b : -((-a + b / 2) / b);
}

void gfx_rgb_to_hsv(uint32_t rgb, int *h, int *s, int *v)
{
    int r = (int)(rgb >> 16 & 0xff), g = (int)(rgb >> 8 & 0xff), b = (int)(rgb & 0xff);
    int max = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int min = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int d = max - min;
    *v = max;
    *s = max ? div_nearest(d * 255, max) : 0;
    if (!d) {
        *h = 0;
        return;
    }
    int hue;
    if (max == r)
        hue = div_nearest(60 * (g - b), d);
    else if (max == g)
        hue = 120 + div_nearest(60 * (b - r), d);
    else
        hue = 240 + div_nearest(60 * (r - g), d);
    *h = (hue % 360 + 360) % 360;
}

uint32_t gfx_hsv_to_rgb(int h, int s, int v)
{
    h = (h % 360 + 360) % 360;
    s = s < 0 ? 0 : s > 255 ? 255 : s;
    v = v < 0 ? 0 : v > 255 ? 255 : v;
    /* The six sectors of 60 degrees: f is the position in the sector. */
    int sector = h / 60, f = (h % 60) * 255;
    int p = div_nearest(v * (255 - s), 255);
    int q = div_nearest(v * (255 * 60 - s * f / 255), 255 * 60);
    int t = div_nearest(v * (255 * 60 - s * (255 * 60 - f) / 255), 255 * 60);
    int r, g, b;
    switch (sector) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    return (uint32_t)(r << 16 | g << 8 | b);
}

int gfx_color_parse(const char *text, uint32_t *rgb)
{
    if (text[0] == '#')
        text++;
    else if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        text += 2;
    uint32_t value = 0;
    int n = 0;
    for (; *text; text++, n++) {
        int c = *text, d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
                           c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0 || n == 6)
            return -1;
        value = value << 4 | (uint32_t)d;
    }
    if (n != 6)
        return -1;
    *rgb = value;
    return 0;
}
