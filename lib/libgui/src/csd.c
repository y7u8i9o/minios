/* Client side decorations: geometry, hit testing and painting of the
 * chrome. The header bar is flat and light, the title centred, the
 * buttons are small circles with symbolic icons, the frame has a
 * hairline outline and a light shadow shifted down. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <gui/paint.h>
#include <gui/pixel.h>
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

/* The chrome around the frame: black with the alpha of the outline, a
 * ring S device pixels wide just outside the frame, over the shadow. Both
 * depend only on the distance of a pixel centre to a rounded rectangle,
 * so they come from two profiles by the squared distance, with 16 bits
 * per value so that only the final alpha is rounded. half_offset
 * gives twice the distance of the centre of pixel x to the interval
 * [lo, hi] of the inner rectangle of a rounded rectangle. With DX and DY
 * of a pixel, D2 = DX * DX + DY * DY is an integer, and the distance of
 * the centre to the rounded rectangle is sqrt(D2) / 2 - R. */
static inline int half_offset(int x, int lo, int hi)
{
    int c = 2 * x + 1;
    return c < 2 * lo ? 2 * lo - c : c > 2 * hi ? c - 2 * hi : 0;
}

struct chrome_profile {
    int S, R, valid;
    int n_outline, n_shadow;
    uint16_t *outline, *shadow;     /* alpha by D2, 65535 for opaque */
};
/* One profile for active windows and one for the others. The thread that
 * draws owns them, the only one in a libgui program. */
static struct chrome_profile profiles[2];

static const struct chrome_profile *chrome_profile(int S, int R, int active)
{
    struct chrome_profile *p = &profiles[active ? 1 : 0];
    if (p->valid && p->S == S && p->R == R)
        return p;
    free(p->outline);
    free(p->shadow);
    p->valid = 0;
    int ro = 2 * (R + S) + 2, rs = 2 * (R + CSD_SHADOW_REACH * S) + 2;
    p->n_outline = ro * ro;
    p->n_shadow = rs * rs;
    p->outline = malloc((size_t)p->n_outline * sizeof p->outline[0]);
    p->shadow = malloc((size_t)p->n_shadow * sizeof p->shadow[0]);
    if (!p->outline || !p->shadow)
        return NULL;
    float ring = (float)S, reach = (float)(CSD_SHADOW_REACH * S);
    float peak = (float)(active ? CSD_SHADOW_PEAK : CSD_SHADOW_PEAK_BACKDROP);
    for (int d2 = 0; d2 < p->n_outline; d2++) {
        float d = sqrtf((float)d2) / 2.0f - (float)R;
        d = d < 0 ? 0 : d;
        float o = d + 0.5f < ring ? 1 : d - 0.5f < ring ? ring - (d - 0.5f) : 0;
        p->outline[d2] = (uint16_t)((float)CSD_OUTLINE_ALPHA / 255.0f * o * 65535.0f + 0.5f);
    }
    for (int d2 = 0; d2 < p->n_shadow; d2++) {
        float d = sqrtf((float)d2) / 2.0f - (float)R;
        d = d < 0 ? 0 : d;
        float t = d >= reach ? 0 : 1.0f - d / reach;
        p->shadow[d2] = (uint16_t)(peak / 255.0f * t * t * 65535.0f + 0.5f);
    }
    p->S = S;
    p->R = R;
    p->valid = 1;
    return p;
}

/* The alpha of the chrome at a device pixel: the outline over the shadow,
 * which lies CSD_SHADOW_DY logical pixels lower. */
static uint32_t chrome_alpha(const struct chrome_profile *p, struct rect F, int x, int y)
{
    int R = p->R, sy = F.y + CSD_SHADOW_DY * p->S;
    long dx = half_offset(x, F.x + R, F.x + F.w - R);
    long dy = half_offset(y, F.y + R, F.y + F.h - R), ds = half_offset(y, sy + R, sy + F.h - R);
    long d2 = dx * dx + dy * dy, d2s = dx * dx + ds * ds;
    uint64_t ao = d2 < p->n_outline ? p->outline[d2] : 0;
    uint64_t as = d2s < p->n_shadow ? p->shadow[d2s] : 0;
    uint64_t a = ao + (as * (65535 - ao) + 32767) / 65535;
    return (uint32_t)((a * 255 + 32767) / 65535);
}

/* An opaque colour with coverage cov (0 to 255) over black chrome of
 * alpha ac, with straight alpha. */
static uint32_t over(uint32_t ac, uint32_t rgb, uint32_t cov)
{
    if (cov >= 255)
        return 0xff000000u | (rgb & 0x00ffffff);
    uint32_t a = cov + pixel_div255(ac * (255 - cov));
    if (!a)
        return 0;
    uint32_t r = ((rgb >> 16 & 0xff) * cov + a / 2) / a;
    uint32_t g = ((rgb >> 8 & 0xff) * cov + a / 2) / a;
    uint32_t b = ((rgb & 0xff) * cov + a / 2) / a;
    return a << 24 | r << 16 | g << 8 | b;
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

static struct rect header_paint(struct surface *buf, int S, const struct csd *c, int w, int h)
{
    fonts();
    struct rect f = csd_frame(c, w, h);
    struct rect hdr = { f.x, f.y, f.w, CSD_HEADER };
    uint32_t bg = c->active ? HEADER_BG : HEADER_BG_BACKDROP;
    uint32_t line = c->active ? HEADER_LINE : HEADER_LINE_BACKDROP;
    uint32_t fg = c->active ? TITLE_FG : TITLE_FG_BACKDROP;
    struct rect dh = rect_scale(hdr, S);
    struct painter p;
    painter_init_scaled(&p, buf, &csd_theme, S);
    /* A window narrower than the button row would otherwise get buttons
     * in the margin, outside the returned damage. */
    p.clip = rect_intersect(p.clip, dh);
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
        struct rect d = rect_scale(b, S);
        gfx_disc(buf, d.x, d.y, d.w, 2 * S, bb, &dh);
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
    return dh;
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
    struct rect F = rect_scale(csd_frame(c, w, h), S);
    const struct chrome_profile *prof = chrome_profile(S, radius(c) * S, c->active);
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
                buf->pixels[(size_t)y * buf->stride + x] = prof ? chrome_alpha(prof, F, x, y) << 24 : 0;
    }
    header_paint(buf, S, c, w, h);
    return all;
}

int csd_corner_rects(const struct csd *c, int w, int h, int scale, struct rect out[4])
{
    int Rd = radius(c) * scale;
    if (!c->enabled || Rd <= 0)
        return 0;
    struct rect F = rect_scale(csd_frame(c, w, h), scale);
    out[0] = (struct rect){ F.x, F.y, Rd, Rd };
    out[1] = (struct rect){ F.x + F.w - Rd, F.y, Rd, Rd };
    out[2] = (struct rect){ F.x, F.y + F.h - Rd, Rd, Rd };
    out[3] = (struct rect){ F.x + F.w - Rd, F.y + F.h - Rd, Rd, Rd };
    return 4;
}

void csd_finish_corners(struct surface *buf, int scale, const struct csd *c, int w, int h)
{
    struct rect corners[4];
    int n = csd_corner_rects(c, w, h, scale, corners);
    if (!n)
        return;
    struct rect F = rect_scale(csd_frame(c, w, h), scale);
    int Rd = corners[0].w;
    const struct chrome_profile *prof = chrome_profile(scale, Rd, c->active);
    const uint8_t *table = pixel_corner_table(Rd);
    for (int k = 0; k < n; k++) {
        struct rect r = rect_intersect(corners[k], (struct rect){ 0, 0, buf->width, buf->height });
        for (int y = r.y; y < r.y + r.h; y++) {
            uint32_t *row = buf->pixels + (size_t)y * buf->stride;
            for (int x = r.x; x < r.x + r.w; x++) {
                uint32_t cov = pixel_round_rect_coverage(table, Rd, F.x, F.y, F.w, F.h, 15, x, y);
                row[x] = over(prof ? chrome_alpha(prof, F, x, y) : 0, row[x], cov);
            }
        }
    }
}
