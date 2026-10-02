/* Scene: the ordered surfaces, damage merging, occlusion culling and
 * per rectangle composition into the back buffer (from the M19 server),
 * plus the cursor. Rectangles, positions and damage are logical pixels;
 * the back buffer holds screen_scale device pixels per logical pixel:
 * buffers with the output's scale are copied 1:1, others are resampled. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "comp.h"

#define MAX_PIECES 64

static struct rect damage[MAX_DAMAGE * 2];
static int ndamage;
static int shown_x, shown_y;            /* where the cursor was drawn */
/* Default arrow cursor: 12x18 shape plus a one pixel drop shadow that
 * keeps 60 percent of the background's brightness. */
#define CURSOR_W 12
#define CURSOR_H 18
#define CURSOR_SHADOW 1
#define CURSOR_SHADE 154
static long stat_count, stat_ms, stat_max;

/* Logical rectangle to device pixels. */
static struct rect dev(struct rect r)
{
    int s = screen_scale;
    return (struct rect){ r.x * s, r.y * s, r.w * s, r.h * s };
}

static inline uint32_t blend_px(uint32_t d, uint32_t c)
{
    uint32_t a = c >> 24;
    if (a == 255)
        return c & 0x00ffffff;
    if (!a)
        return d;
    uint32_t ia = 256 - (a + (a >> 7));
    a += a >> 7;
    return (((d >> 16 & 0xff) * ia + (c >> 16 & 0xff) * a) >> 8) << 16 |
           (((d >> 8 & 0xff) * ia + (c >> 8 & 0xff) * a) >> 8) << 8 |
           (((d & 0xff) * ia + (c & 0xff) * a) >> 8);
}

void scene_init(void)
{
    shown_x = screen_w / 2;
    shown_y = screen_h / 2;
}

static int rects_touch(struct rect a, struct rect b)
{
    return a.x <= b.x + b.w && b.x <= a.x + a.w && a.y <= b.y + b.h && b.y <= a.y + a.h;
}

void scene_damage(struct rect r)
{
    struct rect whole = { 0, 0, screen_w, screen_h };
    r = rect_intersect(r, whole);
    if (rect_empty(r))
        return;
    for (int i = 0; i < ndamage;) {
        if (rects_touch(damage[i], r)) {
            r = rect_union(damage[i], r);
            damage[i] = damage[--ndamage];
            i = 0;
        } else {
            i++;
        }
    }
    if (ndamage == MAX_DAMAGE * 2) {
        for (int i = 1; i < ndamage; i++)
            damage[0] = rect_union(damage[0], damage[i]);
        ndamage = 1;
    }
    damage[ndamage++] = r;
}

void scene_damage_all(void)
{
    struct rect all = { 0, 0, screen_w, screen_h };
    scene_damage(all);
}

int scene_has_damage(void)
{
    return ndamage > 0;
}

static struct rect cursor_rect(void)
{
    struct csurface *s = seat_cursor_surface();
    if (seat_cursor_hidden())
        return (struct rect){ 0, 0, 0, 0 };
    if (s && s->mapped && s->current.buffer)
        return surface_rect(s);
    struct rect r = { shown_x, shown_y, CURSOR_W + CURSOR_SHADOW, CURSOR_H + CURSOR_SHADOW };
    return r;
}

void scene_cursor_changed(void)
{
    struct rect r = cursor_rect();
    if (!rect_empty(r))
        scene_damage(r);
}

void scene_set_cursor(int x, int y)
{
    scene_cursor_changed();
    shown_x = x;
    shown_y = y;
    struct csurface *s = seat_cursor_surface();
    if (s) {
        s->x = x - s->hotspot_x;
        s->y = y - s->hotspot_y;
    }
    scene_cursor_changed();
}

/* Extent drawn for a surface: its decorations frame when it has one. */
static struct rect extent(const struct csurface *s)
{
    return decor_has(s) ? decor_extent(s) : surface_rect(s);
}

static int visible(const struct csurface *s)
{
    if (!s->mapped || !s->current.buffer)
        return 0;
    if (s->role == ROLE_TOPLEVEL && s->toplevel && s->toplevel->minimized)
        return 0;
    if (s->role == ROLE_CURSOR)
        return 0;
    if (s->role == ROLE_IME_POPUP && !im_candidates_visible())
        return 0;
    return 1;
}

/* Sort key: background layers, toplevels by stack, top layers, popups
 * above everything, then drag icons. */
static long sort_key(const struct csurface *s)
{
    switch (s->role) {
    case ROLE_LAYER: return s->layer->layer >= 2 ? 3000000L + s->id : s->id;
    case ROLE_TOPLEVEL: return 1000000L + s->stack;
    case ROLE_POPUP:
        return s->popup && s->popup->parent && s->popup->parent->role == ROLE_LAYER &&
               s->popup->parent->layer->layer >= 2 ? 3500000L + s->id : 2000000L + s->id;
    case ROLE_IME_POPUP: return 4000000L + s->id;
    case ROLE_DND_ICON: return 5000000L + s->id;
    default: return 2000000L + s->id;
    }
}

static int order_cmp(const void *a, const void *b)
{
    long ka = sort_key(*(struct csurface *const *)a), kb = sort_key(*(struct csurface *const *)b);
    return ka < kb ? -1 : ka > kb ? 1 : 0;
}

int scene_order(struct csurface **out, int max)
{
    int n = 0;
    for (struct csurface *s = surface_first(); s && n < max; s = s->next)
        if (visible(s))
            out[n++] = s;
    qsort(out, (size_t)n, sizeof out[0], order_cmp);
    return n;
}

struct csurface *scene_surface_at(int x, int y)
{
    struct csurface *order[256];
    int n = scene_order(order, 256);
    for (int i = n - 1; i >= 0; i--)
        if (order[i]->role != ROLE_DND_ICON && surface_accepts_input(order[i], x, y))
            return order[i];
    return NULL;
}

static int rect_subtract(struct rect a, struct rect b, struct rect out[4])
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

static int pieces_subtract(struct rect *pieces, int *n, struct rect cover)
{
    struct rect out[MAX_PIECES];
    int m = 0;
    for (int i = 0; i < *n; i++) {
        struct rect parts[4];
        int k = rect_subtract(pieces[i], cover, parts);
        if (m + k > MAX_PIECES)
            return 0;
        for (int j = 0; j < k; j++)
            out[m++] = parts[j];
    }
    memcpy(pieces, out, (size_t)m * sizeof out[0]);
    *n = m;
    return 1;
}

/* Blit a surface's buffer clipped to clip (logical screen coordinates). */
static void draw_surface(struct csurface *s, struct rect clip)
{
    struct buffer *b = s->current.buffer;
    if (!b || !b->pool->map)
        return;
    struct rect r = rect_intersect(surface_rect(s), clip);
    if (rect_empty(r))
        return;
    int bs = s->current.scale > 0 ? s->current.scale : 1;
    int S = screen_scale;
    struct rect R = dev(r);
    int ox = s->x * S, oy = s->y * S;               /* the surface's device origin */
    if (bs == S && s->current.transform == 0) {
        /* Rows inside the client's opaque region are copied, the rest
         * is blended (an ARGB buffer with client side shadows). */
        struct rect opaque = { 0, 0, 0, 0 };
        if (b->format == FORMAT_XRGB8888)
            opaque = R;
        else
            for (int q = 0; q < s->current.nopaque; q++) {
                struct rect o = s->current.opaque[q];
                o = dev((struct rect){ o.x + s->x, o.y + s->y, o.w, o.h });
                if (rect_contains(o, R.x, R.y) && rect_contains(o, R.x + R.w - 1, R.y + R.h - 1)) {
                    opaque = R;
                    break;
                }
                struct rect i = rect_intersect(o, R);
                if (i.w * i.h > opaque.w * opaque.h)
                    opaque = i;
            }
        for (int j = 0; j < R.h; j++) {
            int y = R.y + j, sy = y - oy;
            const uint32_t *from = (const uint32_t *)(b->pool->map + b->offset + (size_t)sy * b->stride) + (R.x - ox);
            uint32_t *to = back.pixels + (size_t)y * back.stride + R.x;
            int x0 = 0, x1 = 0;
            if (y >= opaque.y && y < opaque.y + opaque.h) {
                x0 = opaque.x - R.x;
                x1 = x0 + opaque.w;
            }
            if (b->format == FORMAT_XRGB8888) {
                memcpy(to, from, (size_t)R.w * 4);
                continue;
            }
            for (int i = 0; i < x0; i++)
                to[i] = blend_px(to[i], from[i]);
            for (int i = x0; i < x1; i++)
                to[i] = from[i] & 0x00ffffff;
            for (int i = x1 > x0 ? x1 : x0; i < R.w; i++)
                to[i] = blend_px(to[i], from[i]);
        }
        return;
    }
    /* Nearest neighbour through the buffer scale and transform. */
    int bw = b->width, bh = b->height;
    for (int j = 0; j < R.h; j++) {
        uint32_t *to = back.pixels + (size_t)(R.y + j) * back.stride + R.x;
        int v = (R.y - oy + j) * bs / S;
        for (int i = 0; i < R.w; i++) {
            int u = (R.x - ox + i) * bs / S, bx, by;
            switch (s->current.transform) {
            case 1: bx = v; by = bh - bs - u; break;
            case 2: bx = bw - bs - u; by = bh - bs - v; break;
            case 3: bx = bw - bs - v; by = u; break;
            default: bx = u; by = v; break;
            }
            if (bx < 0 || by < 0 || bx >= bw || by >= bh)
                continue;
            uint32_t c = ((const uint32_t *)(b->pool->map + b->offset + (size_t)by * b->stride))[bx];
            to[i] = b->format == FORMAT_XRGB8888 ? (c & 0x00ffffff) : blend_px(to[i], c);
        }
    }
}

static void draw_cursor(struct rect clip)
{
    struct csurface *cursor = seat_cursor_surface();
    if (seat_cursor_hidden())
        return;
    if (cursor && cursor->mapped && cursor->current.buffer) {
        draw_surface(cursor, clip);
        return;
    }
    /* X: black outline, o: white fill, .: transparent. Every cell is an
     * S by S block of device pixels. */
    static const char *shape[CURSOR_H] = {
        "X...........", "XX..........", "XoX.........", "XooX........", "XoooX.......",
        "XooooX......", "XoooooX.....", "XooooooX....", "XoooooooX...", "XooooooooX..",
        "XoooooXXXXX.", "XooXooX.....", "XoX.XooX....", "XX..XooX....", "X....XooX...",
        ".....XooX...", "......XX....", "............",
    };
    int S = screen_scale;
    struct rect area = rect_intersect(cursor_rect(), clip);
    if (rect_empty(area))
        return;
    struct rect R = dev(area);
    for (int Y = R.y; Y < R.y + R.h && Y < back.height; Y++)
        for (int X = R.x; X < R.x + R.w && X < back.width; X++) {
            int i = (X - shown_x * S) / S, j = (Y - shown_y * S) / S;
            uint32_t *dst = &back.pixels[(size_t)Y * back.stride + X];
            char c = i < CURSOR_W && j < CURSOR_H ? shape[j][i] : '.';
            if (c == 'X') {
                *dst = 0x00000000;
            } else if (c == 'o') {
                *dst = 0x00ffffff;
            } else if (i >= CURSOR_SHADOW && j >= CURSOR_SHADOW &&
                       shape[j - CURSOR_SHADOW][i - CURSOR_SHADOW] != '.') {
                /* Drop shadow: the background darkened under the shape
                 * shifted down and right. */
                uint32_t v = *dst;
                *dst = ((((v >> 16) & 0xff) * CURSOR_SHADE / 256) << 16) |
                       ((((v >> 8) & 0xff) * CURSOR_SHADE / 256) << 8) |
                       ((v & 0xff) * CURSOR_SHADE / 256);
            }
        }
}

static void compose_rect(struct rect r, struct csurface **order, int n)
{
    struct rect pieces[MAX_PIECES];
    int np = 1;
    struct rect R = dev(r);
    /* The desktop colour under everything, unless one opaque surface
     * covers the whole rectangle (a frame of a large window). */
    int covered = 0;
    for (int i = 0; i < n && !covered; i++) {
        struct csurface *s = order[i];
        if (!s->current.buffer)
            continue;
        if (s->current.buffer->format == FORMAT_XRGB8888) {
            struct rect o = decor_has(s) ? decor_opaque(s) : surface_rect(s);
            covered = rect_contains(o, r.x, r.y) && rect_contains(o, r.x + r.w - 1, r.y + r.h - 1);
        } else {
            for (int q = 0; q < s->current.nopaque && !covered; q++) {
                struct rect o = s->current.opaque[q];
                o.x += s->x;
                o.y += s->y;
                covered = rect_contains(o, r.x, r.y) && rect_contains(o, r.x + r.w - 1, r.y + r.h - 1);
            }
        }
    }
    if (!covered)
        gfx_fill_rect(&back, R.x, R.y, R.w, R.h, (uint32_t)settings.desktop_color);
    for (int i = 0; i < n; i++) {
        struct csurface *s = order[i];
        pieces[0] = rect_intersect(extent(s), r);
        np = rect_empty(pieces[0]) ? 0 : 1;
        int exact = 1;
        for (int j = i + 1; j < n && np; j++) {
            struct csurface *cover = order[j];
            if (!cover->current.buffer)
                continue;
            if (cover->current.buffer->format == FORMAT_XRGB8888) {
                struct rect opaque = decor_has(cover) ? decor_opaque(cover) : surface_rect(cover);
                if (!pieces_subtract(pieces, &np, opaque)) {
                    exact = 0;
                    break;
                }
            } else {
                for (int q = 0; q < cover->current.nopaque && np; q++) {
                    struct rect o = cover->current.opaque[q];
                    o.x += cover->x;
                    o.y += cover->y;
                    if (!pieces_subtract(pieces, &np, o)) {
                        exact = 0;
                        break;
                    }
                }
                if (!exact)
                    break;
            }
        }
        if (!exact) {
            pieces[0] = rect_intersect(extent(s), r);
            np = 1;
        }
        for (int k = 0; k < np; k++) {
            if (decor_has(s)) {
                decor_draw_shadow(s, pieces[k]);
                decor_draw(s, pieces[k]);
            }
            draw_surface(s, pieces[k]);
            hang_draw(s, pieces[k]);
        }
    }
    ime_draw(r);
    draw_cursor(r);
    backend_flush(r);
}

void scene_compose(void)
{
    if (!ndamage)
        return;
    long t0 = uptime_ms();
    struct csurface *order[256];
    int n = scene_order(order, 256);
    for (int d = 0; d < ndamage; d++)
        compose_rect(damage[d], order, n);
    ndamage = 0;
    long dt = uptime_ms() - t0;
    stat_count++;
    stat_ms += dt;
    if (dt > stat_max)
        stat_max = dt;
    if (dt > 20)
        comp_log("slow frame: %ld ms", dt);
}

void scene_stat_values(long *count, long *ms, long *max)
{
    *count = stat_count;
    *ms = stat_ms;
    *max = stat_max;
}

void scene_stats(void)
{
    comp_log("frame stats: %ld compositions, %ld ms total, %ld ms max", stat_count, stat_ms, stat_max);
}
