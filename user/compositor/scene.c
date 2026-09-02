/* Scene: the ordered surfaces, damage merging, occlusion culling and
 * per rectangle composition into the back buffer (from the M19 server),
 * plus the cursor. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "comp.h"

#define MAX_PIECES 64

static struct rect damage[MAX_DAMAGE * 2];
static int ndamage;
static int shown_x, shown_y;            /* where the cursor was drawn */
static long stat_count, stat_ms, stat_max;

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
    struct rect r = { shown_x, shown_y, 12, 18 };
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

/* Blit a surface's buffer clipped to clip (screen coordinates). */
static void draw_surface(struct csurface *s, struct rect clip)
{
    struct buffer *b = s->current.buffer;
    if (!b || !b->pool->map)
        return;
    struct rect r = rect_intersect(surface_rect(s), clip);
    if (rect_empty(r))
        return;
    int scale = s->current.scale > 0 ? s->current.scale : 1;
    if (scale == 1 && s->current.transform == 0) {
      for (int j = 0; j < r.h; j++) {
        int sy = r.y - s->y + j;
        const uint32_t *from = (const uint32_t *)(b->pool->map + b->offset + (size_t)sy * b->stride) + (r.x - s->x);
        uint32_t *to = back.pixels + (size_t)(r.y + j) * back.stride + r.x;
        if (b->format == FORMAT_XRGB8888) {
            memcpy(to, from, (size_t)r.w * 4);
            continue;
        }
        for (int i = 0; i < r.w; i++) {
            uint32_t c = from[i], a = c >> 24;
            if (a == 255) {
                to[i] = c & 0x00ffffff;
            } else if (a) {
                uint32_t d = to[i], ia = 256 - (a + (a >> 7));
                a += a >> 7;
                to[i] = (((d >> 16 & 0xff) * ia + (c >> 16 & 0xff) * a) >> 8) << 16 |
                        (((d >> 8 & 0xff) * ia + (c >> 8 & 0xff) * a) >> 8) << 8 |
                        (((d & 0xff) * ia + (c & 0xff) * a) >> 8);
            }
        }
      }
      return;
    }
    int ow = b->width / scale, oh = b->height / scale;
    for (int j = 0; j < r.h; j++) {
        uint32_t *to = back.pixels + (size_t)(r.y + j) * back.stride + r.x;
        int ly = r.y - s->y + j;
        for (int i = 0; i < r.w; i++) {
            int lx = r.x - s->x + i, ox, oy;
            switch (s->current.transform) {
            case 1: ox = ly; oy = oh - 1 - lx; break;
            case 2: ox = ow - 1 - lx; oy = oh - 1 - ly; break;
            case 3: ox = ow - 1 - ly; oy = lx; break;
            default: ox = lx; oy = ly; break;
            }
            if (ox < 0 || oy < 0 || ox >= ow || oy >= oh)
                continue;
            const uint32_t *row = (const uint32_t *)(b->pool->map + b->offset + (size_t)(oy * scale) * b->stride);
            uint32_t c = row[ox * scale];
            if (b->format == FORMAT_XRGB8888) {
                to[i] = c & 0x00ffffff;
            } else {
                uint32_t a = c >> 24;
                if (a == 255) to[i] = c & 0x00ffffff;
                else if (a) {
                    uint32_t d = to[i], ia = 256 - (a + (a >> 7));
                    a += a >> 7;
                    to[i] = (((d >> 16 & 0xff) * ia + (c >> 16 & 0xff) * a) >> 8) << 16 |
                            (((d >> 8 & 0xff) * ia + (c >> 8 & 0xff) * a) >> 8) << 8 |
                            (((d & 0xff) * ia + (c & 0xff) * a) >> 8);
                }
            }
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
    static const char *shape[18] = {
        "X...........", "XX..........", "X.X.........", "X..X........", "X...X.......",
        "X....X......", "X.....X.....", "X......X....", "X.......X...", "X........X..",
        "X.....XXXXX.", "X..X..X.....", "X.X.X..X....", "XX..X..X....", "X....X..X...",
        ".....X..X...", "......XX....", "............",
    };
    for (int j = 0; j < 18; j++)
        for (int i = 0; i < 12; i++) {
            int px = shown_x + i, py = shown_y + j;
            if (shape[j][i] == '.' || !rect_contains(clip, px, py) || px >= screen_w || py >= screen_h)
                continue;
            int inside = i > 0 && i < 11 && j > 0 && j < 17 && shape[j][i - 1] != '.' && shape[j][i + 1] != '.' &&
                         shape[j - 1][i] != '.' && shape[j + 1][i] != '.';
            back.pixels[(size_t)py * back.stride + px] = inside ? 0x00ffffff : 0x00000000;
        }
}

static void compose_rect(struct rect r, struct csurface **order, int n)
{
    struct rect pieces[MAX_PIECES];
    int np = 1;
    gfx_fill_rect(&back, r.x, r.y, r.w, r.h, (uint32_t)settings.desktop_color);
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
                struct rect opaque = decor_has(cover) ? decor_frame(cover) : surface_rect(cover);
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
        }
    }
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
