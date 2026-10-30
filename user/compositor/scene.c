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
#include <gui/paint.h>
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

void scene_init(void)
{
    shown_x = screen_w / 2;
    shown_y = screen_h / 2;
    scene_cursor_changed();
}

/* The text that X12 draws itself (the title bars and the message of a
 * locked session without a locker) uses the interface font of the
 * default theme at 13 pixels. */
#define SERVER_FONT_PX 13
static struct theme server_theme;
static const struct font *server_font;

static void load_server_font(void)
{
    if (server_font)
        return;
    theme_init_default(&server_theme);
    struct font *f = gfx_font_open_ttf(server_theme.font_path, SERVER_FONT_PX);
    server_font = f ? f : gfx_font_builtin();
}

const struct font *scene_font(void)
{
    load_server_font();
    return server_font;
}

const struct theme *scene_theme(void)
{
    load_server_font();
    return &server_theme;
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

/* The image of a cursor shape in device pixels, with its hotspot at the
 * pointer (cursor.c). */
static struct rect shape_device_rect(const struct cursor_image *img)
{
    int S = screen_scale;
    return (struct rect){ shown_x * S - img->hot_x, shown_y * S - img->hot_y, img->w, img->h };
}

static int div_down(int a, int b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

/* The logical rectangle of the cursor: the cursor surface of the client,
 * or the logical pixels that the image of the shape touches. */
static struct rect cursor_rect(void)
{
    if (seat_cursor_hidden())
        return (struct rect){ 0, 0, 0, 0 };
    int shape = seat_cursor_shape();
    if (shape < 0)
        return surface_rect(seat_cursor_surface());
    const struct cursor_image *img = cursor_image_of(shape);
    if (!img)
        return (struct rect){ 0, 0, 0, 0 };
    struct rect d = shape_device_rect(img);
    int S = screen_scale, x0 = div_down(d.x, S), y0 = div_down(d.y, S);
    int x1 = div_down(d.x + d.w + S - 1, S), y1 = div_down(d.y + d.h + S - 1, S);
    return (struct rect){ x0, y0, x1 - x0, y1 - y0 };
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
    return wm_extent(s);
}

static int visual_identity(const struct visual *v)
{
    return v->alpha == 255 && v->scale == ANIM_ONE && v->dx == 0 && v->dy == 0;
}

/* The device position of the edge e of a surface after the scale around
 * the centre c and the offset d, all in 16.16 device pixels. */
static int64_t transform_edge(int64_t e, int64_t c, int32_t scale, int d)
{
    return c + ((e - c) * scale >> 16) + ((int64_t)d << 16);
}

static int64_t floor16(int64_t v) { return v >> 16; }
static int64_t ceil16(int64_t v) { return (v + ANIM_ONE - 1) >> 16; }

/* The transformed extent in device pixels: the device pixels that the
 * transformed extent touches. */
static struct rect visual_device_extent(const struct csurface *s)
{
    struct rect E = rect_scale(extent(s), screen_scale);
    int64_t cx = ((int64_t)2 * E.x + E.w) << 15, cy = ((int64_t)2 * E.y + E.h) << 15;
    const struct visual *v = &s->visual;
    int64_t x0 = floor16(transform_edge((int64_t)E.x << 16, cx, v->scale, v->dx));
    int64_t x1 = ceil16(transform_edge((int64_t)(E.x + E.w) << 16, cx, v->scale, v->dx));
    int64_t y0 = floor16(transform_edge((int64_t)E.y << 16, cy, v->scale, v->dy));
    int64_t y1 = ceil16(transform_edge((int64_t)(E.y + E.h) << 16, cy, v->scale, v->dy));
    return (struct rect){ (int)x0, (int)y0, (int)(x1 - x0), (int)(y1 - y0) };
}

struct rect scene_visual_extent(const struct csurface *s)
{
    if (visual_identity(&s->visual))
        return extent(s);
    struct rect d = visual_device_extent(s);
    int S = screen_scale, x0 = div_down(d.x, S), y0 = div_down(d.y, S);
    int x1 = div_down(d.x + d.w + S - 1, S), y1 = div_down(d.y + d.h + S - 1, S);
    return (struct rect){ x0, y0, x1 - x0, y1 - y0 };
}

/* A surface with visual parameters other than the identity is drawn from
 * an image of its extent, scaled, moved and faded. A snapshot is always
 * drawn from its image. Such a surface occludes nothing. */
static int drawn_from_image(const struct csurface *s)
{
    return s->role == ROLE_SNAPSHOT || !visual_identity(&s->visual);
}

/* A minimized toplevel remains visible while its minimize animation
 * runs. */
static int visible(const struct csurface *s)
{
    if (s->role == ROLE_SNAPSHOT)
        return !lock_active();
    if (!s->mapped || !s->current.buffer)
        return 0;
    if (s->role == ROLE_TOPLEVEL && s->toplevel && s->toplevel->minimized && !s->anim.running)
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
    case ROLE_SNAPSHOT: return s->stack;    /* the key of the closed surface */
    default: return 2000000L + s->id;
    }
}

static int order_cmp(const void *a, const void *b)
{
    long ka = sort_key(*(struct csurface *const *)a), kb = sort_key(*(struct csurface *const *)b);
    return ka < kb ? -1 : ka > kb ? 1 : 0;
}

long scene_sort_key(const struct csurface *s)
{
    return sort_key(s);
}

int scene_shows(const struct csurface *s)
{
    return visible(s);
}

int scene_order(struct csurface **out, int max)
{
    int n = 0;
    for (struct csurface *s = surface_first(); s && n < max; s = s->next)
        if (visible(s))
            out[n++] = s;
    for (struct csurface *s = anim_snapshot_first(); s && n < max; s = s->next)
        if (visible(s))
            out[n++] = s;
    qsort(out, (size_t)n, sizeof out[0], order_cmp);
    return n;
}

struct csurface *scene_surface_at(int x, int y)
{
    struct csurface *order[256];
    int n = scene_order(order, 256);
    /* Input acts on the final state of the animations: a snapshot and a
     * minimized toplevel take no input. */
    for (int i = n - 1; i >= 0; i--) {
        struct csurface *s = order[i];
        if (s->role == ROLE_DND_ICON || s->role == ROLE_SNAPSHOT ||
            (s->role == ROLE_TOPLEVEL && s->toplevel && s->toplevel->minimized))
            continue;
        if (surface_accepts_input(s, x, y))
            return s;
    }
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
    if (!cover->current.buffer || drawn_from_image(cover))
        return 1;
    if (cover->current.buffer->format == FORMAT_XRGB8888)
        return pieces_subtract(pieces, np, wm_opaque(cover));
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
    /* The contents of a transformed surface change the whole image. */
    if (drawn_from_image(s)) {
        scene_damage(scene_visual_extent(s));
        return;
    }
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

/* ---- images of surfaces ---- */

/* The extent of s with the identity, rendered into an ARGB image of
 * W x H device pixels with straight alpha (stride in bytes). An undecorated
 * surface whose buffer has the screen scale is copied: an XRGB buffer
 * and the opaque region of an ARGB buffer receive the alpha 255. Every
 * other surface is drawn alone with its extent at the origin of a
 * scratch target, twice: over black into the image and over white into
 * a copy. A pixel with the same value over both backgrounds is opaque.
 * For other pixels the difference is the amount of background that
 * shows through, which gives the alpha of the shadow, the rounded corners
 * and translucent client pixels. The colour is the result over black
 * divided by that alpha. The surface is moved to the origin for the
 * drawing and moved back immediately afterwards. Returns 0 or -ENOMEM. */
static int render_extent(struct csurface *s, uint8_t *to, int stride)
{
    struct rect f = extent(s), area = surface_rect(s);
    int S = screen_scale, W = f.w * S, H = f.h * S;
    struct buffer *b = s->current.buffer;
    int bs = s->current.scale > 0 ? s->current.scale : 1;
    if (b && b->pool->map && bs == S && s->current.transform == 0 && f.x == area.x && f.y == area.y && f.w == area.w &&
        f.h == area.h) {
        uint32_t fill = b->format == FORMAT_XRGB8888 ? 0xff000000u : 0;
        for (int y = 0; y < H; y++) {
            const uint32_t *from = (const uint32_t *)(b->pool->map + b->offset + (size_t)y * b->stride);
            uint32_t *row = (uint32_t *)(to + (size_t)y * stride);
            for (int x = 0; x < W; x++)
                row[x] = from[x] | fill;
        }
        for (int q = 0; q < s->current.nopaque && !fill; q++) {
            struct rect o = rect_intersect(rect_scale(s->current.opaque[q], S), (struct rect){ 0, 0, W, H });
            for (int y = o.y; y < o.y + o.h; y++) {
                uint32_t *row = (uint32_t *)(to + (size_t)y * stride);
                for (int x = o.x; x < o.x + o.w; x++)
                    row[x] |= 0xff000000u;
            }
        }
        return 0;
    }
    uint32_t *white = malloc((size_t)W * H * 4);
    if (!white)
        return -ENOMEM;
    struct surface saved = back;
    struct visual visual = s->visual;
    int sx = s->x, sy = s->y;
    s->visual = VISUAL_IDENTITY;
    s->x -= f.x;
    s->y -= f.y;
    struct rect clip = { 0, 0, f.w, f.h };
    for (int pass = 0; pass < 2; pass++) {
        if (pass)
            back = (struct surface){ white, W, H, W };
        else
            back = (struct surface){ (uint32_t *)to, W, H, stride / 4 };
        gfx_fill_rect(&back, 0, 0, W, H, pass ? 0x00ffffff : 0);
        wm_draw_below(s, clip);
        draw_surface(s, clip);
    }
    back = saved;
    s->visual = visual;
    s->x = sx;
    s->y = sy;
    for (int y = 0; y < H; y++) {
        uint32_t *row = (uint32_t *)(to + (size_t)y * stride);
        const uint32_t *over_white = white + (size_t)y * W;
        for (int x = 0; x < W; x++) {
            uint32_t b = row[x], w = over_white[x];
            if (b == w) {
                row[x] = b | 0xff000000u;
                continue;
            }
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

void scene_image_free(struct scene_image *img)
{
    if (img) {
        free(img->pixels);
        free(img);
    }
}

void scene_surface_gone(struct csurface *s)
{
    scene_image_free(s->image);
    s->image = NULL;
}

/* The image shows the current state of s: the size of the extent, the
 * screen scale, the contents (the commits) and the colours of the
 * decorations. A new title without a commit does not render the image
 * again. The title of a transformed window shows its earlier text until
 * the next commit. */
static int image_current(const struct scene_image *img, const struct csurface *s)
{
    struct rect e = extent(s);
    int S = screen_scale;
    const struct toplevel *t = s->role == ROLE_TOPLEVEL ? s->toplevel : NULL;
    return img->width == e.w * S && img->height == e.h * S && img->scale == S && img->commits == s->commits &&
           (!t || (img->activated == t->activated && img->maximized == t->maximized));
}

/* The image of s for the drawing with its visual parameters, rendered
 * again after a change. NULL without memory or for an empty extent. */
static struct scene_image *surface_image(struct csurface *s)
{
    if (s->role == ROLE_SNAPSHOT)
        return s->image;
    if (s->image && image_current(s->image, s))
        return s->image;
    struct rect e = extent(s);
    int S = screen_scale, W = e.w * S, H = e.h * S;
    if (W <= 0 || H <= 0)
        return NULL;
    struct scene_image *img = s->image;
    if (!img) {
        img = calloc(1, sizeof *img);
        if (!img)
            return NULL;
        s->image = img;
    }
    if (img->width * img->height != W * H || !img->pixels) {
        free(img->pixels);
        img->pixels = malloc((size_t)W * H * 4);
    }
    if (!img->pixels || render_extent(s, (uint8_t *)img->pixels, W * 4) < 0) {
        scene_surface_gone(s);
        return NULL;
    }
    const struct toplevel *t = s->role == ROLE_TOPLEVEL ? s->toplevel : NULL;
    img->width = W;
    img->height = H;
    img->scale = S;
    img->commits = s->commits;
    img->activated = t ? t->activated : 0;
    img->maximized = t ? t->maximized : 0;
    return img;
}

struct scene_image *scene_snapshot(struct csurface *s)
{
    struct rect e = extent(s);
    int S = screen_scale, W = e.w * S, H = e.h * S;
    struct scene_image *img = W > 0 && H > 0 ? calloc(1, sizeof *img) : NULL;
    if (img)
        img->pixels = malloc((size_t)W * H * 4);
    if (!img || !img->pixels || render_extent(s, (uint8_t *)img->pixels, W * 4) < 0) {
        scene_image_free(img);
        return NULL;
    }
    img->width = W;
    img->height = H;
    img->scale = S;
    return img;
}

/* Draw the image of s with its visual parameters inside clip (logical
 * pixels). The destination pixel X maps to the image position
 * c + (X + 1/2 - d - c) / scale - 1/2, in device pixels of the extent at
 * the image scale, with c the centre of the extent and d the offset. A
 * scale of 1 with the screen scale of the image copies the rows with the
 * alpha (a fade, or a move by whole device pixels). Every other scale
 * samples bilinearly. The dimming of an unresponsive client
 * (wm_draw_above) is left out during the transform. */
static void draw_image(struct csurface *s, struct rect clip)
{
    const struct visual *v = &s->visual;
    if (!v->alpha || v->scale < 256)
        return;
    int S = screen_scale;
    struct rect D = rect_intersect(rect_intersect(visual_device_extent(s), rect_scale(clip, S)),
                                   (struct rect){ 0, 0, back.width, back.height });
    if (rect_empty(D))
        return;
    struct scene_image *img = surface_image(s);
    if (!img)
        return;
    struct rect E = rect_scale(extent(s), S);
    if (v->scale == ANIM_ONE && img->scale == S) {
        int ox = E.x + v->dx, oy = E.y + v->dy;
        int x0 = MAX(D.x, ox), x1 = MIN(D.x + D.w, ox + img->width);
        for (int y = D.y; y < D.y + D.h && x1 > x0; y++) {
            int iy = y - oy;
            if (iy >= 0 && iy < img->height)
                pixel_over_alpha(back.pixels + (size_t)y * back.stride + x0,
                                 img->pixels + (size_t)iy * img->width + (x0 - ox), x1 - x0, v->alpha);
        }
        return;
    }
    int64_t cx = ((int64_t)2 * E.x + E.w) << 15, cy = ((int64_t)2 * E.y + E.h) << 15;
    int64_t inv = ((int64_t)1 << 32) / v->scale, Si = img->scale;
    int64_t du = inv * Si / S;
    int64_t ux = cx + (((((int64_t)D.x << 16) + 32768 - ((int64_t)v->dx << 16) - cx) * inv) >> 16);
    int64_t iu = (ux - ((int64_t)E.x << 16)) * Si / S - 32768;
    uint32_t tmp[256];
    for (int y = D.y; y < D.y + D.h; y++) {
        int64_t vy = cy + (((((int64_t)y << 16) + 32768 - ((int64_t)v->dy << 16) - cy) * inv) >> 16);
        int64_t iv = (vy - ((int64_t)E.y << 16)) * Si / S - 32768;
        uint32_t *to = back.pixels + (size_t)y * back.stride + D.x;
        for (int i = 0; i < D.w; i += 256) {
            int k = MIN(256, D.w - i);
            pixel_sample_bilinear(tmp, img->pixels, img->width, img->width, img->height, (int32_t)(iu + i * du),
                                  (int32_t)iv, (int32_t)du, k, 0);
            pixel_over_alpha(to + i, tmp, k, v->alpha);
        }
    }
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
    int shape = seat_cursor_shape();
    if (shape < 0) {
        struct csurface *s = seat_cursor_surface();
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
    const struct cursor_image *img = cursor_image_of(shape);
    if (!img || img->w > FB_CURSOR_MAX || img->h > FB_CURSOR_MAX)
        return -1;
    for (int y = 0; y < img->h; y++)
        memcpy(out + y * FB_CURSOR_MAX, img->pixels + (size_t)y * img->w, (size_t)img->w * 4);
    *w = img->w;
    *h = img->h;
    *hot_x = img->hot_x;
    *hot_y = img->hot_y;
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
    int draw = cursor_override >= 0 ? cursor_override : !hw_cursor;
    if (seat_cursor_hidden() || !draw)
        return;
    int shape = seat_cursor_shape();
    if (shape < 0) {
        draw_surface(seat_cursor_surface(), clip);
        return;
    }
    const struct cursor_image *img = cursor_image_of(shape);
    if (!img)
        return;
    struct rect d = shape_device_rect(img);
    struct rect R = rect_intersect(rect_intersect(rect_scale(clip, screen_scale), d),
                                   (struct rect){ 0, 0, back.width, back.height });
    if (rect_empty(R))
        return;
    for (int Y = R.y; Y < R.y + R.h; Y++)
        pixel_over(&back.pixels[(size_t)Y * back.stride + R.x],
                   img->pixels + (size_t)(Y - d.y) * img->w + (R.x - d.x), R.w);
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
        if (!s->current.buffer || drawn_from_image(s))
            continue;
        if (s->current.buffer->format == FORMAT_XRGB8888) {
            struct rect o = wm_opaque(s);
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
        struct rect area = scene_visual_extent(s);
        pieces[0] = rect_intersect(area, r);
        np = rect_empty(pieces[0]) ? 0 : 1;
        int exact = 1;
        for (int j = i + 1; j < n && np; j++)
            if (!subtract_opaque(pieces, &np, order[j])) {
                exact = 0;
                break;
            }
        if (!exact) {
            pieces[0] = rect_intersect(area, r);
            np = 1;
        }
        for (int k = 0; k < np; k++) {
            if (drawn_from_image(s)) {
                draw_image(s, pieces[k]);
                continue;
            }
            wm_draw_below(s, pieces[k]);
            draw_surface(s, pieces[k]);
            wm_draw_above(s, pieces[k]);
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
    /* The image of a surface whose animation ended, or of a hidden
     * surface, is no longer drawn. */
    for (struct csurface *s = surface_first(); s; s = s->next)
        if (s->image && (!drawn_from_image(s) || !visible(s)))
            scene_surface_gone(s);
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

/* The toplevel with the identity, as render_extent draws it. */
int scene_render_window(struct toplevel *t, uint8_t *to, int stride)
{
    return render_extent(t->s, to, stride);
}
