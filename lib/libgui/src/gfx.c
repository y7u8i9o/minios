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
