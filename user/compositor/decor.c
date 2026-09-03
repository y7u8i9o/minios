/* Server side decorations of toplevels: a light title bar with rounded
 * top corners, the title in the interface font, round close, maximize
 * and minimize buttons, a hairline border, a soft drop shadow, invisible
 * resize margins around the frame, and the move and resize drags (also
 * started by a client's move and resize requests). Everything is drawn
 * at the screen scale, antialiased where shapes are curved. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gui/paint.h>
#include "comp.h"

/* Colours of the active window, then of inactive windows. */
#define COLOR_TITLE              0x00e9ecf0
#define COLOR_TEXT               0x0025282d
#define COLOR_BORDER             0x00a4aab3
#define COLOR_SEPARATOR          0x00d2d6dc
#define COLOR_CLOSE              0x00e5484d
#define COLOR_BUTTON             0x00c6ccd4
#define COLOR_GLYPH              0x003a3f46
#define COLOR_GLYPH_CLOSE        0x00ffffff
#define COLOR_TITLE_INACTIVE     0x00f4f5f7
#define COLOR_TEXT_INACTIVE      0x009aa0a8
#define COLOR_BORDER_INACTIVE    0x00c3c8cf
#define COLOR_SEPARATOR_INACTIVE 0x00e2e5e9
#define COLOR_BUTTON_INACTIVE    0x00dfe2e6
#define COLOR_GLYPH_INACTIVE     0x00a6abb3
/* Peak darkening of the shadow, out of 256. */
#define SHADOW_ALPHA          120
#define SHADOW_ALPHA_INACTIVE 72
/* Size of the corner zones where a resize takes two edges. */
#define CORNER 16
#define TITLE_PX 13

static struct toplevel *drag;
static int drag_dx, drag_dy;
static int by_client;                   /* the drag was requested by the client */
static struct toplevel *resizing;
static int resize_edges, resize_x0, resize_y0, resize_w0, resize_h0, resize_sx, resize_sy;

int decor_has(const struct csurface *s)
{
    return s->role == ROLE_TOPLEVEL && s->toplevel && s->toplevel->decor_mode == DECOR_SERVER;
}

struct rect decor_frame(const struct csurface *s)
{
    struct rect r = { s->x - BORDER, s->y - TITLE_H - BORDER, s->width + 2 * BORDER, s->height + TITLE_H + 2 * BORDER };
    return r;
}

struct rect decor_extent(const struct csurface *s)
{
    struct rect r = decor_frame(s);
    if (!s->toplevel->maximized) {
        r.x -= SHADOW;
        r.y -= SHADOW;
        r.w += 2 * SHADOW;
        r.h += 2 * SHADOW + SHADOW_DY;
    }
    return r;
}

struct rect decor_opaque(const struct csurface *s)
{
    struct rect r = decor_frame(s);
    if (!s->toplevel->maximized) {
        r.y += RADIUS;
        r.h -= RADIUS;
    }
    return r;
}

static int radius(const struct csurface *s)
{
    return s->toplevel->maximized ? 0 : RADIUS;
}

static struct rect dev(struct rect r)
{
    int S = screen_scale;
    return (struct rect){ r.x * S, r.y * S, r.w * S, r.h * S };
}

static struct rect clip_dev(struct rect logical)
{
    return rect_intersect(dev(logical), (struct rect){ 0, 0, back.width, back.height });
}

static inline uint32_t mix(uint32_t d, uint32_t c, float t)
{
    if (t >= 1.0f)
        return c;
    int a = (int)(t * 256.0f + 0.5f), ia = 256 - a;
    return ((((d >> 16) & 0xff) * ia + ((c >> 16) & 0xff) * a) >> 8) << 16 |
           ((((d >> 8) & 0xff) * ia + ((c >> 8) & 0xff) * a) >> 8) << 8 |
           (((d & 0xff) * ia + (c & 0xff) * a) >> 8);
}

static inline uint32_t darken(uint32_t c, int a)
{
    uint32_t ia = (uint32_t)(256 - a);
    return ((((c >> 16) & 0xff) * ia) >> 8) << 16 | ((((c >> 8) & 0xff) * ia) >> 8) << 8 | (((c & 0xff) * ia) >> 8);
}

/* Coverage (0..1) of the device pixel (x, y) by the rectangle F whose
 * two top corners are rounded with radius r. */
static float round_coverage(int x, int y, struct rect F, float r)
{
    if (!rect_contains(F, x, y))
        return 0;
    float px = (float)x + 0.5f, py = (float)y + 0.5f, cx, cy;
    if (r <= 0)
        return 1;
    if (py > (float)F.y + r)
        return 1;
    if (px < (float)F.x + r)
        cx = (float)F.x + r;
    else if (px > (float)(F.x + F.w) - r)
        cx = (float)(F.x + F.w) - r;
    else
        return 1;
    cy = (float)F.y + r;
    float dx = px - cx, dy = py - cy;
    float c = r + 0.5f - sqrtf(dx * dx + dy * dy);
    return c < 0 ? 0 : c > 1 ? 1 : c;
}

/* a minus b, as up to four rectangles. */
static int rect_minus(struct rect a, struct rect b, struct rect out[4])
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

/* Shadow strength by the squared distance (device pixels) to the
 * rounded shape's inner rectangle: (1 - t)^2 out of 255 with t the
 * distance beyond the corner radius over the reach. */
static uint8_t *shade;
static int shade_scale, shade_n;

static void shade_table(int S)
{
    if (shade_scale == S)
        return;
    int R = RADIUS * S, reach = SHADOW * S, n = R + reach + 1;
    shade_n = n * n;
    shade = realloc(shade, (size_t)shade_n);
    if (!shade) {
        shade_n = 0;
        return;
    }
    for (int d2 = 0; d2 < shade_n; d2++) {
        float d = sqrtf((float)d2) - (float)R;
        float t = d <= 0 ? 0 : d / (float)reach;
        shade[d2] = t >= 1 ? 0 : (uint8_t)((1.0f - t) * (1.0f - t) * 255.0f);
    }
    shade_scale = S;
}

/* The shadow over one device rectangle outside the opaque frame. Pixels
 * of the frame's corner squares are shaded only where the arc leaves
 * them uncovered, the bar's antialiased edge blends over them. */
static void shade_rect(struct rect r, struct rect F, struct rect shape, int R, int peak)
{
    int x0 = shape.x + R, x1 = shape.x + shape.w - 1 - R, y0 = shape.y + R, y1 = shape.y + shape.h - 1 - R;
    float Rf = (float)R;
    for (int y = r.y; y < r.y + r.h; y++) {
        int dy = y < y0 ? y0 - y : y > y1 ? y - y1 : 0;
        int in_top = y >= F.y && y < F.y + R;
        uint32_t *row = &back.pixels[(size_t)y * back.stride];
        for (int x = r.x; x < r.x + r.w; x++) {
            if (in_top && x >= F.x && x < F.x + F.w) {
                int corner = x < F.x + R || x >= F.x + F.w - R;
                if (!corner || round_coverage(x, y, F, Rf) >= 1)
                    continue;
            }
            int dx = x < x0 ? x0 - x : x > x1 ? x - x1 : 0;
            int d2 = dx * dx + dy * dy;
            if (d2 >= shade_n)
                continue;
            int a = shade[d2] * peak >> 8;
            if (a)
                row[x] = darken(row[x], a);
        }
    }
}

void decor_draw_shadow(struct csurface *s, struct rect clip)
{
    if (!decor_has(s) || s->toplevel->maximized)
        return;
    int S = screen_scale;
    shade_table(S);
    if (!shade)
        return;
    struct rect F = dev(decor_frame(s));
    struct rect shape = { F.x, F.y + SHADOW_DY * S, F.w, F.h };
    struct rect area = clip_dev(rect_intersect(decor_extent(s), clip));
    /* Only the band outside the frame's opaque part needs work; the
     * contents of a window never do. */
    struct rect pieces[4];
    int n = rect_minus(area, dev(decor_opaque(s)), pieces);
    int peak = s->toplevel->activated ? SHADOW_ALPHA : SHADOW_ALPHA_INACTIVE;
    for (int i = 0; i < n; i++)
        if (!rect_empty(pieces[i]))
            shade_rect(pieces[i], F, shape, RADIUS * S, peak);
}

/* Buttons from the right: 0 close, 1 maximize, 2 minimize. */
static struct rect button_rect(const struct csurface *s, int n)
{
    struct rect r = { s->x + s->width - 16 - n * (TITLE_BTN + 4), s->y - TITLE_H + (TITLE_H - TITLE_BTN) / 2, TITLE_BTN,
                      TITLE_BTN };
    return r;
}

static struct rect grip_rect(const struct csurface *s)
{
    struct rect r = { s->x + s->width - GRIP, s->y + s->height - GRIP, GRIP, GRIP };
    return r;
}

/* An antialiased disc inside the logical rect b, inset by one pixel. */
static void draw_disc(struct rect b, struct rect clip, uint32_t color)
{
    int S = screen_scale;
    struct rect r = clip_dev(rect_intersect(b, clip));
    float cx = (float)(b.x * S) + (float)(b.w * S) / 2.0f, cy = (float)(b.y * S) + (float)(b.h * S) / 2.0f;
    float rad = (float)((b.w - 2) * S) / 2.0f;
    for (int y = r.y; y < r.y + r.h; y++)
        for (int x = r.x; x < r.x + r.w; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            float c = rad + 0.5f - sqrtf(dx * dx + dy * dy);
            if (c <= 0)
                continue;
            uint32_t *p = &back.pixels[(size_t)y * back.stride + x];
            *p = mix(*p, color, c);
        }
}

/* The title is drawn through a painter at the screen scale with the
 * interface font, so it is as sharp as the clients' text. */
static struct theme decor_theme;
static const struct font *title_font;

static void decor_fonts(void)
{
    if (title_font)
        return;
    theme_init_default(&decor_theme);
    struct font *f = gfx_font_open_ttf(decor_theme.font_path, TITLE_PX);
    title_font = f ? f : gfx_font_builtin();
}

/* Loading and first rasterizing the font takes a noticeable time, so it
 * happens at startup rather than when the first window maps. */
void decor_init(void)
{
    decor_fonts();
    gfx_text_width_font_scaled(title_font, "The quick brown fox jumps over the lazy dog 0123456789", -1, screen_scale);
}

void decor_draw(struct csurface *s, struct rect clip)
{
    struct rect frame = decor_frame(s);
    struct rect c = rect_intersect(frame, clip);
    if (rect_empty(c))
        return;
    decor_fonts();
    int S = screen_scale;
    int active = s->toplevel->activated;
    uint32_t bar = active ? COLOR_TITLE : COLOR_TITLE_INACTIVE;
    uint32_t border = active ? COLOR_BORDER : COLOR_BORDER_INACTIVE;
    uint32_t text = active ? COLOR_TEXT : COLOR_TEXT_INACTIVE;
    uint32_t separator = active ? COLOR_SEPARATOR : COLOR_SEPARATOR_INACTIVE;

    /* The corner squares of the title bar: the outer rounded shape in
     * the border colour, the bar inset by the border, blended over the
     * shadow. The rest of the bar and the border are plain fills. */
    struct rect F = dev(frame), inner = { F.x + S, F.y + S, F.w - 2 * S, F.h - S };
    int rad = radius(s), Rd = rad * S;
    float R = (float)Rd;
    for (int side = 0; side < 2 && rad > 0; side++) {
        struct rect q = { side ? F.x + F.w - Rd : F.x, F.y, Rd, Rd };
        q = rect_intersect(q, clip_dev(c));
        for (int y = q.y; y < q.y + q.h; y++)
            for (int x = q.x; x < q.x + q.w; x++) {
                float outer = round_coverage(x, y, F, R);
                if (outer <= 0)
                    continue;
                uint32_t *p = &back.pixels[(size_t)y * back.stride + x];
                *p = mix(*p, border, outer);
                float in = round_coverage(x, y, inner, R > (float)S ? R - (float)S : 0);
                if (in > 0)
                    *p = mix(*p, bar, in);
            }
    }

    struct painter p;
    painter_init_scaled(&p, &back, &decor_theme, S);
    painter_push(&p, c.x, c.y, c.w, c.h);
    int ox = -c.x, oy = -c.y;
    int bar_h = TITLE_H + BORDER;
    painter_fill(&p, frame.x + rad + ox, frame.y + oy, frame.w - 2 * rad, rad, bar);
    painter_fill(&p, frame.x + ox, frame.y + rad + oy, frame.w, bar_h - rad, bar);
    painter_fill(&p, frame.x + rad + ox, frame.y + oy, frame.w - 2 * rad, BORDER, border);
    painter_fill(&p, frame.x + ox, frame.y + rad + oy, BORDER, frame.h - rad, border);
    painter_fill(&p, frame.x + frame.w - BORDER + ox, frame.y + rad + oy, BORDER, frame.h - rad, border);
    painter_fill(&p, frame.x + ox, frame.y + frame.h - BORDER + oy, frame.w, BORDER, border);
    /* Separator above the contents. */
    painter_fill(&p, s->x + ox, s->y - 1 + oy, s->width, 1, separator);

    /* The title, centred when it fits between the left edge and the
     * buttons, else left aligned and clipped. */
    int left = s->x + 10, right = button_rect(s, 2).x - 10;
    if (right > left && s->toplevel->title[0]) {
        int tw = (gfx_text_width_font_scaled(title_font, s->toplevel->title, -1, S) + S - 1) / S;
        int tx = s->x + (s->width - tw) / 2;
        if (tx < left || tx + tw > right)
            tx = left;
        int ty = s->y - TITLE_H + (TITLE_H - title_font->height) / 2;
        painter_push(&p, left + ox, s->y - TITLE_H + oy, right - left, TITLE_H);
        painter_text_font(&p, title_font, tx - left, ty - (s->y - TITLE_H), s->toplevel->title, text, 0xffffffffu);
        painter_pop(&p);
    }

    /* Round buttons with their glyphs. */
    for (int n = 0; n < 3; n++) {
        struct rect b = button_rect(s, n);
        if (rect_empty(rect_intersect(b, c)))
            continue;
        uint32_t fill = !active ? COLOR_BUTTON_INACTIVE : n == 0 ? COLOR_CLOSE : COLOR_BUTTON;
        uint32_t glyph = !active ? COLOR_GLYPH_INACTIVE : n == 0 ? COLOR_GLYPH_CLOSE : COLOR_GLYPH;
        draw_disc(b, c, fill);
        int bx = b.x + ox, by = b.y + oy;
        if (n == 0) {
            painter_line(&p, bx + 4, by + 4, bx + 9, by + 9, glyph);
            painter_line(&p, bx + 9, by + 4, bx + 4, by + 9, glyph);
        } else if (n == 1) {
            painter_frame(&p, bx + 4, by + 4, 6, 6, glyph);
        } else {
            painter_fill(&p, bx + 4, by + 7, 6, 1, glyph);
        }
    }
    painter_pop(&p);
}

/* Edges of a resize started at (x, y): the invisible margins around the
 * frame, with corner zones taking two edges. 0 on the title bar. */
static int edges_at(const struct csurface *s, int x, int y)
{
    struct rect f = decor_frame(s);
    int e = 0;
    if (x < s->x) e |= EDGE_LEFT;
    if (x >= s->x + s->width) e |= EDGE_RIGHT;
    if (y < f.y + BORDER) e |= EDGE_TOP;
    if (y >= s->y + s->height) e |= EDGE_BOTTOM;
    if (!e)
        return 0;
    if (e & (EDGE_LEFT | EDGE_RIGHT)) {
        if (y < f.y + CORNER) e |= EDGE_TOP;
        if (y >= f.y + f.h - CORNER) e |= EDGE_BOTTOM;
    }
    if (e & (EDGE_TOP | EDGE_BOTTOM)) {
        if (x < f.x + CORNER) e |= EDGE_LEFT;
        if (x >= f.x + f.w - CORNER) e |= EDGE_RIGHT;
    }
    return e;
}

static void start_resize(struct toplevel *t, int edges)
{
    resizing = t;
    resize_edges = edges;
    resize_x0 = cursor_x;
    resize_y0 = cursor_y;
    toplevel_configure_size(t, &resize_w0, &resize_h0);
    resize_sx = t->s->x;
    resize_sy = t->s->y;
}

static void start_move(struct toplevel *t)
{
    drag = t;
    drag_dx = cursor_x - t->s->x;
    drag_dy = cursor_y - t->s->y;
}

/* button: 1 = left with the pointer on the decorations; bit 0x100 marks
 * a move requested by the client, 0x200 a resize with the edges in
 * bits 16 and up. */
int decor_press(struct csurface *s, int button)
{
    struct toplevel *t = s->toplevel;
    if (!t)
        return 0;
    if (button & 0x100) {
        start_move(t);
        by_client = 1;
        return 1;
    }
    if (button & 0x200) {
        start_resize(t, button >> 16);
        by_client = 1;
        return 1;
    }
    by_client = 0;
    if (!decor_has(s) || !(button & 1))
        return 0;
    if (seat_modifiers() & 4) {                 /* Alt drag from anywhere */
        if (!t->maximized)
            start_move(t);
        return 1;
    }
    int edges = t->maximized ? 0 : edges_at(s, cursor_x, cursor_y);
    if (edges) {
        start_resize(t, edges);
        return 1;
    }
    if (cursor_y < s->y) {
        if (rect_contains(button_rect(s, 0), cursor_x, cursor_y))
            toplevel_close(t);
        else if (rect_contains(button_rect(s, 1), cursor_x, cursor_y))
            toplevel_set_maximized(t, !t->maximized);
        else if (rect_contains(button_rect(s, 2), cursor_x, cursor_y))
            toplevel_set_minimized(t, 1);
        else if (!t->maximized)
            start_move(t);
        return 1;
    }
    if (!t->maximized && rect_contains(grip_rect(s), cursor_x, cursor_y)) {
        start_resize(t, EDGE_BOTTOM | EDGE_RIGHT);
        return 1;
    }
    return 0;
}

int decor_dragging(void)
{
    return drag != NULL || resizing != NULL;
}

int decor_motion(void)
{
    if (drag) {
        toplevel_move(drag, cursor_x - drag_dx, cursor_y - drag_dy);
        return 1;
    }
    return resizing != NULL;
}

/* Returns 1 when a server started drag ended, 2 when a client started
 * one did (the client still gets the release), 0 otherwise. */
int decor_release(void)
{
    int done = by_client ? 2 : 1;
    if (drag) {
        drag = NULL;
        return done;
    }
    if (resizing) {
        struct toplevel *t = resizing;
        resizing = NULL;
        int dx = cursor_x - resize_x0, dy = cursor_y - resize_y0;
        int w = resize_w0, h = resize_h0;
        if (resize_edges & EDGE_RIGHT) w += dx;
        if (resize_edges & EDGE_LEFT) { w -= dx; t->s->x = resize_sx + dx; }
        if (resize_edges & EDGE_BOTTOM) h += dy;
        if (resize_edges & EDGE_TOP) { h -= dy; t->s->y = resize_sy + dy; }
        if (w < 32) w = 32;
        if (h < 32) h = 32;
        toplevel_configure(t, w, h);
        return done;
    }
    return 0;
}

/* Whether the point lies on the decorations of s (not its contents):
 * the frame, the resize margins around it and the grip. */
int decor_hit(const struct csurface *s, int x, int y);
int decor_hit(const struct csurface *s, int x, int y)
{
    if (!decor_has(s))
        return 0;
    if (rect_contains(surface_rect(s), x, y))
        return !s->toplevel->maximized && rect_contains(grip_rect(s), x, y);
    struct rect zone = decor_frame(s);
    if (!s->toplevel->maximized) {
        zone.x -= RESIZE_MARGIN;
        zone.y -= RESIZE_MARGIN;
        zone.w += 2 * RESIZE_MARGIN;
        zone.h += 2 * RESIZE_MARGIN;
    }
    return rect_contains(zone, x, y);
}
