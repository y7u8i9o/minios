/* Scene: the ordered surfaces, damage merging, occlusion culling and
 * per rectangle composition into the back buffer (from the M19 server),
 * plus the cursor. Rectangles, positions and damage are logical pixels;
 * the back buffer contains screen_scale device pixels per logical pixel:
 * buffers with the output's scale are copied 1:1, others are resampled. */
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/param.h>
#include <minios/abi.h>
#include <gui/pixel.h>
#include "comp.h"

#define MAX_PIECES 64

/* The damage since the last frame, disjoint rectangles (gui/gfx.h). */
static struct rect_set damage;
static int shown_x, shown_y;            /* the cursor position, logical pixels */
/* The device cursor (G9 of docs/plan/compositor-performance.md). On a
 * device with FB_CAP_CURSOR the device shows the cursor image above the
 * frames. A pointer motion then moves the device cursor and composes
 * nothing. A cursor image larger than FB_CURSOR_MAX device pixels is
 * drawn into the frames, as on a device without the capability. */
static int hw_cursor;                   /* the device shows the cursor, the frames contain none */
static uint32_t hw_image[FB_CURSOR_MAX * FB_CURSOR_MAX];
static int hw_w, hw_h, hw_hot_x, hw_hot_y;  /* the image of the device, hw_w 0 when hidden */
/* -1 for the frames. 0 or 1 while a screen copy composes without or with
 * the cursor. */
static int cursor_override = -1;
/* Default arrow cursor: 12x18 shape plus a one pixel drop shadow that
 * retains 60 percent of the background's brightness. */
#define CURSOR_W 12
#define CURSOR_H 18
#define CURSOR_SHADOW 1
#define CURSOR_KEEP 153

void scene_init(void)
{
    shown_x = screen_w / 2;
    shown_y = screen_h / 2;
    scene_cursor_changed();
}

void scene_damage(struct rect r)
{
    struct rect whole = { 0, 0, screen_w, screen_h };
    r = rect_intersect(r, whole);
    if (rect_empty(r))
        return;
    stats_mark(STATS_DAMAGE);
    rect_set_add(&damage, r);
    overlay_note_damage(r);
}

void scene_damage_all(void)
{
    struct rect all = { 0, 0, screen_w, screen_h };
    scene_damage(all);
}

int scene_has_damage(void)
{
    return damage.n > 0;
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

static void cursor_sync(void);

void scene_cursor_changed(void)
{
    cursor_sync();
    struct rect r = cursor_rect();
    if (!hw_cursor && !rect_empty(r))
        scene_damage(r);
}

void scene_set_cursor(int x, int y)
{
    if (!hw_cursor)
        scene_cursor_changed();
    shown_x = x;
    shown_y = y;
    struct csurface *s = seat_cursor_surface();
    if (s) {
        s->x = x - s->hotspot_x;
        s->y = y - s->hotspot_y;
    }
    if (hw_cursor) {
        long t0 = uptime_us();
        backend_cursor_move(x * screen_scale, y * screen_scale);
        stats_cursor_move(t0);
    } else {
        scene_cursor_changed();
    }
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
    /* A locked session shows only the lock surface and its popups. */
    if (lock_active()) {
        struct csurface *view = lock_surface_mapped();
        return view && (s == view || (s->role == ROLE_POPUP && s->popup && s->popup->parent == view));
    }
    return 1;
}

/* Sort key: background layers, toplevels by stack, top layers, popups
 * above everything, overlay layers (the screenshot interface) above the
 * popups of the panel, then the input method candidates and drag icons. */
static long sort_key(const struct csurface *s)
{
    switch (s->role) {
    case ROLE_LAYER:
        if (s->layer->layer == LAYER_OVERLAY)
            return 3800000L + s->id;
        return s->layer->layer >= 2 ? 3000000L + s->id : s->id;
    case ROLE_TOPLEVEL: return 1000000L + s->stack;
    case ROLE_POPUP:
        if (s->popup && s->popup->parent && s->popup->parent->role == ROLE_LOCK)
            return 6500000L + s->id;
        return s->popup && s->popup->parent && s->popup->parent->role == ROLE_LAYER &&
               s->popup->parent->layer->layer >= 2 ? 3500000L + s->id : 2000000L + s->id;
    case ROLE_IME_POPUP: return 4000000L + s->id;
    case ROLE_LOCK: return 6000000L + s->id;
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

/* The opaque area of a surface above others: the frame of a decorated
 * window or an XRGB surface, else the opaque region of an ARGB buffer.
 * Removes it from the pieces. Returns 0 when the pieces become too many. */
static int subtract_opaque(struct rect *pieces, int *np, const struct csurface *cover)
{
    if (!cover->current.buffer)
        return 1;
    if (cover->current.buffer->format == FORMAT_XRGB8888)
        return pieces_subtract(pieces, np, decor_has(cover) ? decor_opaque(cover) : surface_rect(cover));
    for (int q = 0; q < cover->current.nopaque && *np; q++) {
        struct rect o = cover->current.opaque[q];
        o.x += cover->x;
        o.y += cover->y;
        if (!pieces_subtract(pieces, np, o))
            return 0;
    }
    return 1;
}

void scene_damage_surface(const struct csurface *s, struct rect r)
{
    struct csurface *order[256];
    int n = scene_order(order, 256), i = 0;
    while (i < n && order[i] != s)
        i++;
    struct rect pieces[MAX_PIECES] = { r };
    int np = 1;
    for (int j = i + 1; j < n && np; j++)
        if (!subtract_opaque(pieces, &np, order[j])) {
            scene_damage(r);
            return;
        }
    for (int k = 0; k < np; k++)
        scene_damage(pieces[k]);
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
    struct rect R = rect_scale(r, screen_scale);
    int ox = s->x * S, oy = s->y * S;               /* the surface's device origin */
    if (bs == S && s->current.transform == 0) {
        /* Pixels inside the client's opaque region are copied, the rest
         * is blended (an ARGB buffer with client side shadows). The
         * rectangles of the region may overlap. */
        struct rect opaque[MAX_REGION];
        int nopaque = 0;
        if (b->format != FORMAT_XRGB8888)
            for (int q = 0; q < s->current.nopaque; q++) {
                struct rect o = s->current.opaque[q];
                o = rect_intersect(rect_scale((struct rect){ o.x + s->x, o.y + s->y, o.w, o.h }, S), R);
                if (!rect_empty(o))
                    opaque[nopaque++] = o;
            }
        for (int j = 0; j < R.h; j++) {
            int y = R.y + j, sy = y - oy;
            const uint32_t *from = (const uint32_t *)(b->pool->map + b->offset + (size_t)sy * b->stride) + (R.x - ox);
            uint32_t *to = back.pixels + (size_t)y * back.stride + R.x;
            if (b->format == FORMAT_XRGB8888) {
                memcpy(to, from, (size_t)R.w * 4);
                continue;
            }
            /* The opaque spans of the row in the order of their start. */
            int start[MAX_REGION], end[MAX_REGION], nspans = 0;
            for (int q = 0; q < nopaque; q++) {
                if (y < opaque[q].y || y >= opaque[q].y + opaque[q].h)
                    continue;
                int k = nspans++;
                for (; k > 0 && start[k - 1] > opaque[q].x - R.x; k--) {
                    start[k] = start[k - 1];
                    end[k] = end[k - 1];
                }
                start[k] = opaque[q].x - R.x;
                end[k] = start[k] + opaque[q].w;
            }
            int at = 0;
            for (int k = 0; k < nspans; k++) {
                if (end[k] <= at)
                    continue;
                int from_x = start[k] > at ? start[k] : at;
                pixel_over(to + at, from + at, from_x - at);
                pixel_copy_opaque(to + from_x, from + from_x, end[k] - from_x);
                at = end[k];
            }
            pixel_over(to + at, from + at, R.w - at);
        }
        return;
    }
    /* Nearest neighbour through the buffer scale: the buffer pixel u of
     * the device pixel x is floor((x - ox) * bs / S), which a walk gives
     * without a division per pixel. */
    int bw = b->width, bh = b->height;
    const uint8_t *pix = b->pool->map + b->offset;
    if (s->current.transform == 0) {
        for (int j = 0; j < R.h; j++) {
            uint32_t *to = back.pixels + (size_t)(R.y + j) * back.stride + R.x;
            int v = (R.y - oy + j) * bs / S;
            if (v < 0 || v >= bh)
                continue;
            const uint32_t *row = (const uint32_t *)(pix + (size_t)v * b->stride);
            struct pixel_walk w;
            pixel_walk_init(&w, R.x - ox, bs, S);
            if (b->format == FORMAT_XRGB8888) {
                pixel_sample(to, row, 1, R.w, &w);
                pixel_copy_opaque(to, to, R.w);
            } else {
                pixel_sample_over(to, row, 1, R.w, &w);
            }
        }
        return;
    }
    /* The rotated and flipped transforms, which no client of minios uses,
     * pixel by pixel. */
    for (int j = 0; j < R.h; j++) {
        uint32_t *to = back.pixels + (size_t)(R.y + j) * back.stride + R.x;
        int v = (R.y - oy + j) * bs / S;
        struct pixel_walk w;
        pixel_walk_init(&w, R.x - ox, bs, S);
        for (int i = 0; i < R.w; i++, pixel_walk_next(&w)) {
            int u = (int)w.pos, bx, by;
            switch (s->current.transform) {
            case 1: bx = v; by = bh - bs - u; break;
            case 2: bx = bw - bs - u; by = bh - bs - v; break;
            default: bx = bw - bs - v; by = u; break;
            }
            if (bx < 0 || by < 0 || bx >= bw || by >= bh)
                continue;
            uint32_t c = ((const uint32_t *)(pix + (size_t)by * b->stride))[bx];
            if (b->format == FORMAT_XRGB8888)
                to[i] = c & 0x00ffffff;
            else if (c >> 24)
                to[i] = pixel_blend(to[i], c, c >> 24);
        }
    }
}

/* The default arrow at the screen scale as an ARGB image of
 * (CURSOR_W + CURSOR_SHADOW) by (CURSOR_H + CURSOR_SHADOW) cells of S by S
 * device pixels: X is the black outline, o the white fill, and the shape
 * shifted down and right is the shadow, black with the alpha that leaves
 * CURSOR_KEEP / 255 of the background. */
static const uint32_t *arrow_image(void)
{
    static const char *shape[CURSOR_H] = {
        "X...........", "XX..........", "XoX.........", "XooX........", "XoooX.......",
        "XooooX......", "XoooooX.....", "XooooooX....", "XoooooooX...", "XooooooooX..",
        "XoooooXXXXX.", "XooXooX.....", "XoX.XooX....", "XX..XooX....", "X....XooX...",
        ".....XooX...", "......XX....", "............",
    };
    static uint32_t *img;
    static int img_scale;
    int S = screen_scale, W = (CURSOR_W + CURSOR_SHADOW) * S, H = (CURSOR_H + CURSOR_SHADOW) * S;
    if (img && img_scale == S)
        return img;
    free(img);
    img = malloc((size_t)W * H * 4);
    if (!img)
        return NULL;
    img_scale = S;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            int i = x / S, j = y / S;
            char c = i < CURSOR_W && j < CURSOR_H ? shape[j][i] : '.';
            uint32_t v = 0;
            if (c == 'X')
                v = 0xff000000u;
            else if (c == 'o')
                v = 0xffffffffu;
            else if (i >= CURSOR_SHADOW && j >= CURSOR_SHADOW && shape[j - CURSOR_SHADOW][i - CURSOR_SHADOW] != '.')
                v = (255u - CURSOR_KEEP) << 24;
            img[(size_t)y * W + x] = v;
        }
    return img;
}

/* The cursor as device pixels in rows of FB_CURSOR_MAX pixels, with the
 * hotspot. Returns 0 for a hidden cursor (w 0) and for an image that fits
 * FB_CURSOR_MAX. Returns -1 for a larger or transformed cursor surface. */
static int cursor_image(uint32_t *out, int *w, int *h, int *hot_x, int *hot_y)
{
    int S = screen_scale;
    *w = *h = *hot_x = *hot_y = 0;
    if (seat_cursor_hidden())
        return 0;
    struct csurface *s = seat_cursor_surface();
    if (s && s->mapped && s->current.buffer) {
        const struct buffer *b = s->current.buffer;
        int W = s->width * S, H = s->height * S;
        if (!b->pool->map || s->current.transform || W <= 0 || H <= 0 || W > FB_CURSOR_MAX || H > FB_CURSOR_MAX)
            return -1;
        /* The nearest buffer pixel. The buffer may have another scale
         * than the screen. */
        for (int y = 0; y < H; y++) {
            const uint32_t *row =
                (const uint32_t *)(b->pool->map + b->offset + (size_t)(y * b->height / H) * b->stride);
            for (int x = 0; x < W; x++) {
                uint32_t c = row[x * b->width / W];
                out[y * FB_CURSOR_MAX + x] = b->format == FORMAT_XRGB8888 ? c | 0xff000000u : c;
            }
        }
        *w = W;
        *h = H;
        *hot_x = MIN(MAX(s->hotspot_x * S, 0), W - 1);
        *hot_y = MIN(MAX(s->hotspot_y * S, 0), H - 1);
        return 0;
    }
    const uint32_t *img = arrow_image();
    int W = (CURSOR_W + CURSOR_SHADOW) * S, H = (CURSOR_H + CURSOR_SHADOW) * S;
    if (!img || W > FB_CURSOR_MAX || H > FB_CURSOR_MAX)
        return -1;
    for (int y = 0; y < H; y++)
        memcpy(out + y * FB_CURSOR_MAX, img + (size_t)y * W, (size_t)W * 4);
    *w = W;
    *h = H;
    return 0;
}

/* Give the device the current cursor image. When the image does not fit
 * or the device refuses it, the device cursor is hidden and the frames
 * contain the cursor again. A change between the two damages the cursor
 * rectangle. */
static void cursor_sync(void)
{
    if (!backend_has_cursor())
        return;
    static uint32_t img[FB_CURSOR_MAX * FB_CURSOR_MAX];
    memset(img, 0, sizeof img);
    int w, h, hot_x, hot_y;
    int fits = cursor_image(img, &w, &h, &hot_x, &hot_y) == 0;
    if (fits && hw_cursor && w == hw_w && h == hw_h && hot_x == hw_hot_x && hot_y == hw_hot_y &&
        memcmp(img, hw_image, sizeof img) == 0)
        return;
    int was = hw_cursor, S = screen_scale;
    if (fits && backend_cursor_set(img, w, h, FB_CURSOR_MAX, hot_x, hot_y, shown_x * S, shown_y * S) == 0) {
        hw_cursor = 1;
        memcpy(hw_image, img, sizeof img);
        hw_w = w;
        hw_h = h;
        hw_hot_x = hot_x;
        hw_hot_y = hot_y;
    } else {
        if (hw_cursor && hw_w)
            backend_cursor_set(img, 0, 0, FB_CURSOR_MAX, 0, 0, 0, 0);
        hw_cursor = 0;
        hw_w = 0;
    }
    struct rect r = cursor_rect();
    if (was != hw_cursor && !rect_empty(r))
        scene_damage(r);
}

static void draw_cursor(struct rect clip)
{
    struct csurface *cursor = seat_cursor_surface();
    int draw = cursor_override >= 0 ? cursor_override : !hw_cursor;
    if (seat_cursor_hidden() || !draw)
        return;
    if (cursor && cursor->mapped && cursor->current.buffer) {
        draw_surface(cursor, clip);
        return;
    }
    const uint32_t *img = arrow_image();
    if (!img)
        return;
    int S = screen_scale, W = (CURSOR_W + CURSOR_SHADOW) * S;
    struct rect area = rect_intersect(cursor_rect(), clip);
    if (rect_empty(area))
        return;
    struct rect R = rect_scale(area, S);
    for (int Y = R.y; Y < R.y + R.h; Y++)
        pixel_over(&back.pixels[(size_t)Y * back.stride + R.x],
                   img + (size_t)(Y - shown_y * S) * W + (R.x - shown_x * S), R.w);
}

static void compose_rect(struct rect r, struct csurface **order, int n)
{
    struct rect pieces[MAX_PIECES];
    int np = 1;
    struct rect R = rect_scale(r, screen_scale);
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
    /* A locked session has a black background. */
    if (!covered)
        gfx_fill_rect(&back, R.x, R.y, R.w, R.h, lock_active() ? 0 : (uint32_t)settings.desktop_color);
    for (int i = 0; i < n; i++) {
        struct csurface *s = order[i];
        pieces[0] = rect_intersect(extent(s), r);
        np = rect_empty(pieces[0]) ? 0 : 1;
        int exact = 1;
        for (int j = i + 1; j < n && np; j++)
            if (!subtract_opaque(pieces, &np, order[j])) {
                exact = 0;
                break;
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
    lock_draw(r);
    overlay_draw(r, order, n);
    draw_cursor(r);
}

void scene_compose(void)
{
    if (!damage.n)
        return;
    overlay_frame_begin();
    long t0 = stats_frame_begin();
    struct csurface *order[256];
    int n = scene_order(order, 256);
    for (int d = 0; d < damage.n; d++) {
        struct rect R = rect_scale(damage.r[d], screen_scale);
        compose_rect(damage.r[d], order, n);
        stats_rect((long)R.w * R.h);
    }
    backend_present(damage.r, damage.n);
    rect_set_clear(&damage);
    long dt = stats_frame_end(t0);
    if (dt > 20000)
        comp_log("slow frame: %ld us", dt);
}

/* ---- screen capture ---- */

struct rect scene_pointer_rect(void)
{
    struct rect r = cursor_rect();
    if (rect_empty(r))
        return r;
    return rect_intersect(rect_scale(r, screen_scale), (struct rect){ 0, 0, back.width, back.height });
}

/* The back buffer contains the last composed frame. It contains the
 * cursor unless the device shows it. When the copy needs the other
 * state, the rectangle under the pointer is composed again with or
 * without the cursor, the buffer is copied, and the rectangle is
 * composed once more as before. Nothing is flushed in between, so the
 * screen never shows the changed rectangle. */
void scene_copy_screen(uint8_t *to, int stride, int pointer)
{
    int drawn = !hw_cursor;
    struct rect under = pointer == drawn ? (struct rect){ 0, 0, 0, 0 } : cursor_rect();
    struct csurface *order[256];
    int n = 0;
    if (!rect_empty(under)) {
        n = scene_order(order, 256);
        cursor_override = pointer;
        compose_rect(under, order, n);
        cursor_override = -1;
    }
    for (int y = 0; y < back.height; y++) {
        const uint32_t *from = back.pixels + (size_t)y * back.stride;
        uint32_t *row = (uint32_t *)(to + (size_t)y * stride);
        for (int x = 0; x < back.width; x++)
            row[x] = from[x] | 0xff000000u;
    }
    if (!rect_empty(under))
        compose_rect(under, order, n);
}

struct rect scene_window_extent(const struct toplevel *t)
{
    return extent(t->s);
}

/* The toplevel is drawn alone, with its extent at the origin of a scratch
 * target, twice: over black into the client buffer and over white into a
 * copy. A pixel with the same value over both backgrounds is opaque. For
 * other pixels the difference is the amount of background that shows
 * through, which gives the alpha of the shadow, the rounded corners and
 * translucent client pixels. The colour is the result over black divided
 * by that alpha. The surface is moved to the origin for the drawing and
 * moved back immediately afterwards. */
int scene_render_window(struct toplevel *t, uint8_t *to, int stride)
{
    struct csurface *s = t->s;
    struct rect f = extent(s);
    int S = screen_scale, W = f.w * S, H = f.h * S;
    uint32_t *white = malloc((size_t)W * H * 4);
    if (!white)
        return -ENOMEM;
    struct surface saved = back;
    int sx = s->x, sy = s->y;
    s->x -= f.x;
    s->y -= f.y;
    struct rect clip = { 0, 0, f.w, f.h };
    for (int pass = 0; pass < 2; pass++) {
        if (pass)
            back = (struct surface){ white, W, H, W };
        else
            back = (struct surface){ (uint32_t *)to, W, H, stride / 4 };
        gfx_fill_rect(&back, 0, 0, W, H, pass ? 0x00ffffff : 0);
        if (decor_has(s)) {
            decor_draw_shadow(s, clip);
            decor_draw(s, clip);
        }
        draw_surface(s, clip);
    }
    back = saved;
    s->x = sx;
    s->y = sy;
    for (int y = 0; y < H; y++) {
        uint32_t *row = (uint32_t *)(to + (size_t)y * stride);
        const uint32_t *over_white = white + (size_t)y * W;
        for (int x = 0; x < W; x++) {
            uint32_t b = row[x], w = over_white[x];
            int show = 0;
            for (int sh = 0; sh < 24; sh += 8) {
                int d = (int)(w >> sh & 0xff) - (int)(b >> sh & 0xff);
                if (d > show)
                    show = d;
            }
            uint32_t a = 255u - (uint32_t)show;
            if (!a) {
                row[x] = 0;
                continue;
            }
            uint32_t c = a << 24;
            for (int sh = 0; sh < 24; sh += 8) {
                uint32_t v = (b >> sh & 0xff) * 255u / a;
                c |= (v > 255 ? 255 : v) << sh;
            }
            row[x] = c;
        }
    }
    free(white);
    return 0;
}
