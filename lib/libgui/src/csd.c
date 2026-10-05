/* Client side decorations: geometry, hit testing and painting of the
 * chrome. The header bar is flat and light, the title centred, the
 * buttons are small circles with symbolic icons, the frame has a
 * hairline outline and a light shadow shifted down. */
#include <math.h>
#include <string.h>
#include <gui/paint.h>
#include "csd.h"

#define HEADER_BG          0x00ebebeb
#define HEADER_BG_BACKDROP 0x00fafafa
#define HEADER_LINE        0x00d4d4d4
#define HEADER_LINE_BACKDROP 0x00e1e1e1
#define TITLE_FG           0x00323232
#define TITLE_FG_BACKDROP  0x00929595
#define BUTTON_BG          0x00d8d8d8
#define BUTTON_BG_HOVER    0x00c9c9c9
#define BUTTON_BG_BACKDROP 0x00e6e6e6
#define TITLE_PX 13
/* Shadow: reach in logical pixels, downward shift, peak alpha out of 255. */
#define SHADOW_REACH 8
#define SHADOW_DY 2
#define SHADOW_PEAK 64
#define SHADOW_PEAK_BACKDROP 32
#define OUTLINE_ALPHA 51       /* 20 percent black */

int csd_margin(const struct csd *c)
{
    return c->enabled && !c->maximized ? CSD_MARGIN : 0;
}

int csd_header(const struct csd *c)
{
    return c->enabled ? CSD_HEADER : 0;
}

static int radius(const struct csd *c)
{
    return c->enabled && !c->maximized ? CSD_RADIUS : 0;
}

void csd_buffer_size(const struct csd *c, int w, int h, int *bw, int *bh)
{
    int m = csd_margin(c);
    *bw = w + 2 * m;
    *bh = h + csd_header(c) + 2 * m;
}

struct rect csd_frame(const struct csd *c, int w, int h)
{
    int m = csd_margin(c);
    return (struct rect){ m, m, w, h + csd_header(c) };
}

struct rect csd_content(const struct csd *c, int w, int h)
{
    int m = csd_margin(c);
    return (struct rect){ m, m + csd_header(c), w, h };
}

/* Buttons from the right: 0 close, 1 maximize, 2 minimize. */
static struct rect button_rect(const struct csd *c, int w, int h, int n)
{
    struct rect f = csd_frame(c, w, h);
    return (struct rect){ f.x + f.w - 8 - CSD_BUTTON - n * (CSD_BUTTON + 6), f.y + (CSD_HEADER - 1 - CSD_BUTTON) / 2,
                          CSD_BUTTON, CSD_BUTTON };
}

/* The resize zones of the frame (gui_resize_edges): the edges outside the
 * frame, and corner squares that reach outside and over the rounded
 * corners inside. */
static const struct gui_resize_zones zones = {
    .margin = CSD_BORDER_ZONE, .inner = 0, .corner = CSD_CORNER, .reach = CSD_CORNER_REACH,
    .inset_top = CSD_RADIUS, .inset_bottom = CSD_RADIUS,
};

enum csd_zone csd_hit(const struct csd *c, int w, int h, int x, int y, int *edges)
{
    *edges = 0;
    if (!c->enabled)
        return CSD_CONTENT;
    struct rect f = csd_frame(c, w, h);
    int e = c->maximized ? 0 : gui_resize_edges(f, &zones, x, y);
    if (e) {
        *edges = e;
        return CSD_RESIZE;
    }
    if (rect_contains(csd_content(c, w, h), x, y))
        return CSD_CONTENT;
    if (rect_contains(f, x, y)) {
        for (int n = 0; n < 3; n++)
            if (rect_contains(button_rect(c, w, h, n), x, y))
                return n == 0 ? CSD_CLOSE : n == 1 ? CSD_MAXIMIZE : CSD_MINIMIZE;
        return CSD_HEADER_BAR;
    }
    return CSD_OUTSIDE;
}

int csd_opaque_region(const struct csd *c, int w, int h, struct rect out[4])
{
    struct rect f = csd_frame(c, w, h);
    int r = radius(c);
    if (r == 0) {
        out[0] = f;
        return 1;
    }
    out[0] = (struct rect){ f.x + r, f.y, f.w - 2 * r, f.h };
    out[1] = (struct rect){ f.x, f.y + r, r, f.h - 2 * r };
    out[2] = (struct rect){ f.x + f.w - r, f.y + r, r, f.h - 2 * r };
    return 3;
}

int csd_input_region(const struct csd *c, int w, int h, struct rect out[5])
{
    struct rect f = csd_frame(c, w, h);
    if (c->maximized) {
        out[0] = f;
        return 1;
    }
    return gui_resize_region(f, &zones, out);
}

/* ---- painting ---- */

static struct rect dev(struct rect r, int s)
{
    return (struct rect){ r.x * s, r.y * s, r.w * s, r.h * s };
}

/* Coverage (0..1) of the device pixel (x, y) by the rectangle F with all
 * four corners rounded by r. */
static float cover(int x, int y, struct rect F, float r)
{
    if (!rect_contains(F, x, y))
        return 0;
    if (r <= 0)
        return 1;
    float px = (float)x + 0.5f, py = (float)y + 0.5f, cx, cy;
    if (px < (float)F.x + r) cx = (float)F.x + r;
    else if (px > (float)(F.x + F.w) - r) cx = (float)(F.x + F.w) - r;
    else return 1;
    if (py < (float)F.y + r) cy = (float)F.y + r;
    else if (py > (float)(F.y + F.h) - r) cy = (float)(F.y + F.h) - r;
    else return 1;
    float dx = px - cx, dy = py - cy;
    float v = r + 0.5f - sqrtf(dx * dx + dy * dy);
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

/* Distance (device pixels, 0 inside) from a pixel centre to the rounded
 * rectangle F. */
static float distance(int x, int y, struct rect F, float r)
{
    float px = (float)x + 0.5f, py = (float)y + 0.5f;
    float x0 = (float)F.x + r, y0 = (float)F.y + r, x1 = (float)(F.x + F.w) - r, y1 = (float)(F.y + F.h) - r;
    float dx = px < x0 ? x0 - px : px > x1 ? px - x1 : 0;
    float dy = py < y0 ? y0 - py : py > y1 ? py - y1 : 0;
    float d = sqrtf(dx * dx + dy * dy) - r;
    return d < 0 ? 0 : d;
}

/* The chrome under and around the frame at a device pixel: black with
 * the alpha of the outline (a ring one logical pixel wide just outside
 * the frame) over the shadow. */
static uint32_t chrome_px(int x, int y, struct rect F, float R, int S, int active)
{
    float d = distance(x, y, F, R);
    float ring = (float)S;
    float o = d + 0.5f < ring ? 1 : d - 0.5f < ring ? ring - (d - 0.5f) : 0;
    struct rect shadow = { F.x, F.y + SHADOW_DY * S, F.w, F.h };
    float ds = distance(x, y, shadow, R), reach = (float)(SHADOW_REACH * S);
    float t = ds >= reach ? 0 : 1.0f - ds / reach;
    float as = (float)(active ? SHADOW_PEAK : SHADOW_PEAK_BACKDROP) / 255.0f * t * t;
    float ao = (float)OUTLINE_ALPHA / 255.0f * o;
    float a = ao + as * (1.0f - ao);
    int ai = (int)(a * 255.0f + 0.5f);
    return (uint32_t)(ai < 0 ? 0 : ai > 255 ? 255 : ai) << 24;
}

/* An opaque colour with coverage cov over the chrome pixel (straight alpha). */
static uint32_t over(uint32_t chrome, uint32_t rgb, float cov)
{
    if (cov >= 1)
        return 0xff000000u | (rgb & 0x00ffffff);
    float ac = (float)(chrome >> 24) / 255.0f;
    float a = cov + ac * (1.0f - cov);
    if (a <= 0)
        return 0;
    float k = cov / a;
    uint32_t r = (uint32_t)((float)((rgb >> 16) & 0xff) * k + 0.5f);
    uint32_t g = (uint32_t)((float)((rgb >> 8) & 0xff) * k + 0.5f);
    uint32_t b = (uint32_t)((float)(rgb & 0xff) * k + 0.5f);
    return (uint32_t)(a * 255.0f + 0.5f) << 24 | r << 16 | g << 8 | b;
}

static const struct font *title_font;
static struct theme csd_theme;

static void fonts(void)
{
    if (title_font)
        return;
    theme_init_default(&csd_theme);
    struct font *f = gfx_font_open_ttf(csd_theme.font_path, TITLE_PX);
    title_font = f ? f : gfx_font_builtin();
}

/* A disc of the button's diameter less two pixels, antialiased, drawn
 * onto the header colour. */
static void disc(struct surface *buf, int S, struct rect b, uint32_t color, uint32_t bg)
{
    struct rect r = rect_intersect(dev(b, S), (struct rect){ 0, 0, buf->width, buf->height });
    float cx = (float)(b.x * S) + (float)(b.w * S) / 2.0f, cy = (float)(b.y * S) + (float)(b.h * S) / 2.0f;
    float rad = (float)((b.w - 2) * S) / 2.0f;
    for (int y = r.y; y < r.y + r.h; y++)
        for (int x = r.x; x < r.x + r.w; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy;
            float c = rad + 0.5f - sqrtf(dx * dx + dy * dy);
            if (c <= 0)
                continue;
            int a = c >= 1 ? 256 : (int)(c * 256.0f), ia = 256 - a;
            uint32_t *p = &buf->pixels[(size_t)y * buf->stride + x];
            *p = ((((bg >> 16) & 0xff) * ia + ((color >> 16) & 0xff) * a) >> 8) << 16 |
                 ((((bg >> 8) & 0xff) * ia + ((color >> 8) & 0xff) * a) >> 8) << 8 |
                 (((bg & 0xff) * ia + (color & 0xff) * a) >> 8);
        }
}

static struct rect header_paint(struct surface *buf, int S, const struct csd *c, int w, int h)
{
    fonts();
    struct rect f = csd_frame(c, w, h);
    struct rect hdr = { f.x, f.y, f.w, CSD_HEADER };
    uint32_t bg = c->active ? HEADER_BG : HEADER_BG_BACKDROP;
    uint32_t line = c->active ? HEADER_LINE : HEADER_LINE_BACKDROP;
    uint32_t fg = c->active ? TITLE_FG : TITLE_FG_BACKDROP;
    struct painter p;
    painter_init_scaled(&p, buf, &csd_theme, S);
    painter_fill(&p, hdr.x, hdr.y, hdr.w, hdr.h - 1, bg);
    painter_fill(&p, hdr.x, hdr.y + hdr.h - 1, hdr.w, 1, line);
    /* Title: centred, clipped between the left edge and the buttons. */
    int right = button_rect(c, w, h, 2).x - 12, left = hdr.x + 12;
    if (right > left && c->title[0]) {
        int tw = (gfx_text_width_font_scaled(title_font, c->title, -1, S) + S - 1) / S;
        int tx = hdr.x + (hdr.w - tw) / 2;
        if (tx < left || tx + tw > right)
            tx = left;
        int ty = hdr.y + (CSD_HEADER - 1 - title_font->height) / 2;
        painter_push(&p, left, hdr.y, right - left, hdr.h);
        int dx, dy;
        struct surface v = { buf->pixels + (size_t)p.clip.y * buf->stride + p.clip.x, p.clip.w, p.clip.h, buf->stride };
        dx = p.ox - p.clip.x;
        dy = p.oy - p.clip.y;
        if (v.width > 0 && v.height > 0)
            gfx_text_font_scaled(&v, title_font, (tx - left) * S + dx, (ty - hdr.y) * S + dy, c->title, fg, 0xffffffffu, S);
        painter_pop(&p);
    }
    for (int n = 0; n < 3; n++) {
        struct rect b = button_rect(c, w, h, n);
        int zone = n == 0 ? CSD_CLOSE : n == 1 ? CSD_MAXIMIZE : CSD_MINIMIZE;
        uint32_t bb = !c->active ? BUTTON_BG_BACKDROP : c->hover == zone ? BUTTON_BG_HOVER : BUTTON_BG;
        disc(buf, S, b, bb, bg);
        int cx = b.x + b.w / 2, cy = b.y + b.h / 2;
        if (n == 0) {
            painter_line(&p, cx - 3, cy - 3, cx + 2, cy + 2, fg);
            painter_line(&p, cx + 2, cy - 3, cx - 3, cy + 2, fg);
        } else if (n == 1) {
            if (c->maximized) {
                painter_frame(&p, cx - 3, cy - 1, 5, 5, fg);
                painter_fill(&p, cx - 1, cy - 3, 5, 1, fg);
                painter_fill(&p, cx + 2, cy - 3, 1, 5, fg);
            } else {
                painter_frame(&p, cx - 3, cy - 3, 6, 6, fg);
            }
        } else {
            painter_fill(&p, cx - 3, cy + 1, 6, 1, fg);
        }
    }
    return dev(hdr, S);
}

struct rect csd_paint_header(struct surface *buf, int scale, const struct csd *c, int w, int h)
{
    if (!c->enabled)
        return (struct rect){ 0, 0, 0, 0 };
    return header_paint(buf, scale, c, w, h);
}

struct rect csd_paint(struct surface *buf, int scale, const struct csd *c, int w, int h)
{
    if (!c->enabled)
        return (struct rect){ 0, 0, 0, 0 };
    int S = scale;
    struct rect F = dev(csd_frame(c, w, h), S);
    float R = (float)(radius(c) * S);
    struct rect all = { 0, 0, buf->width, buf->height };
    /* The margins: shadow and outline, transparent further out. */
    struct rect band[4] = {
        { 0, 0, all.w, F.y }, { 0, F.y + F.h, all.w, all.h - (F.y + F.h) },
        { 0, F.y, F.x, F.h }, { F.x + F.w, F.y, all.w - (F.x + F.w), F.h },
    };
    for (int i = 0; i < 4; i++) {
        struct rect r = rect_intersect(band[i], all);
        for (int y = r.y; y < r.y + r.h; y++)
            for (int x = r.x; x < r.x + r.w; x++)
                buf->pixels[(size_t)y * buf->stride + x] = chrome_px(x, y, F, R, S, c->active);
    }
    header_paint(buf, S, c, w, h);
    return all;
}

void csd_copy(struct surface *dst, const struct surface *src, struct rect r, int scale, const struct csd *c, int w, int h)
{
    r = rect_intersect(r, (struct rect){ 0, 0, src->width, src->height });
    r = rect_intersect(r, (struct rect){ 0, 0, dst->width, dst->height });
    if (rect_empty(r))
        return;
    if (!c->enabled) {
        gfx_copy_rect(dst, src, &r);
        return;
    }
    int S = scale;
    struct rect F = dev(csd_frame(c, w, h), S);
    int Rd = radius(c) * S;
    float R = (float)Rd;
    for (int y = r.y; y < r.y + r.h; y++) {
        const uint32_t *from = src->pixels + (size_t)y * src->stride;
        uint32_t *to = dst->pixels + (size_t)y * dst->stride;
        int in_row = y >= F.y && y < F.y + F.h;
        int x0 = in_row ? (F.x > r.x ? F.x : r.x) : r.x + r.w;
        int x1 = in_row ? (F.x + F.w < r.x + r.w ? F.x + F.w : r.x + r.w) : r.x + r.w;
        if (x1 < x0) x1 = x0;
        /* Chrome outside the frame: as painted. */
        if (x0 > r.x)
            memcpy(to + r.x, from + r.x, (size_t)(x0 - r.x) * 4);
        if (r.x + r.w > x1)
            memcpy(to + x1, from + x1, (size_t)(r.x + r.w - x1) * 4);
        if (x1 <= x0)
            continue;
        int corner_row = Rd > 0 && (y < F.y + Rd || y >= F.y + F.h - Rd);
        for (int x = x0; x < x1; x++) {
            if (corner_row && (x < F.x + Rd || x >= F.x + F.w - Rd)) {
                float cov = cover(x, y, F, R);
                to[x] = over(chrome_px(x, y, F, R, S, c->active), from[x], cov);
            } else {
                to[x] = from[x] | 0xff000000u;
            }
        }
    }
}
