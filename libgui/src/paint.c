/* Painter: origin translation and clipping over gfx primitives. */
#include <gui/paint.h>
#include <string.h>

void painter_init(struct painter *p, struct surface *s, const struct theme *theme)
{
    memset(p, 0, sizeof *p);
    p->s = s;
    p->theme = theme;
    p->clip = (struct rect){ 0, 0, s->width, s->height };
}

void painter_push(struct painter *p, int x, int y, int w, int h)
{
    if (p->depth < PAINTER_DEPTH) {
        p->stack[p->depth] = p->clip;
        p->ostack[p->depth][0] = p->ox;
        p->ostack[p->depth][1] = p->oy;
    }
    p->depth++;
    struct rect r = { p->ox + x, p->oy + y, w, h };
    p->clip = rect_intersect(p->clip, r);
    p->ox += x;
    p->oy += y;
}

void painter_pop(struct painter *p)
{
    if (p->depth == 0)
        return;
    p->depth--;
    if (p->depth < PAINTER_DEPTH) {
        p->clip = p->stack[p->depth];
        p->ox = p->ostack[p->depth][0];
        p->oy = p->ostack[p->depth][1];
    }
}

/* A view of the surface limited to the clip, with local coordinates
 * translated into it. */
static struct surface view(const struct painter *p, int *dx, int *dy)
{
    struct surface v = { p->s->pixels + (size_t)p->clip.y * p->s->stride + p->clip.x, p->clip.w, p->clip.h, p->s->stride };
    if (rect_empty(p->clip))
        v.width = v.height = 0;
    *dx = p->ox - p->clip.x;
    *dy = p->oy - p->clip.y;
    return v;
}

struct rect painter_clip_local(const struct painter *p)
{
    struct rect r = { p->clip.x - p->ox, p->clip.y - p->oy, p->clip.w, p->clip.h };
    return r;
}

void painter_fill(struct painter *p, int x, int y, int w, int h, uint32_t color)
{
    int dx, dy;
    struct surface v = view(p, &dx, &dy);
    gfx_fill_rect(&v, x + dx, y + dy, w, h, color);
}

void painter_frame(struct painter *p, int x, int y, int w, int h, uint32_t color)
{
    int dx, dy;
    struct surface v = view(p, &dx, &dy);
    gfx_rect(&v, x + dx, y + dy, w, h, color);
}

void painter_line(struct painter *p, int x0, int y0, int x1, int y1, uint32_t color)
{
    int dx, dy;
    struct surface v = view(p, &dx, &dy);
    gfx_line(&v, x0 + dx, y0 + dy, x1 + dx, y1 + dy, color);
}

void painter_rounded(struct painter *p, int x, int y, int w, int h, uint32_t fill, uint32_t border)
{
    int r = theme_px(p->theme, TM_RADIUS);
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    int dx, dy;
    struct surface v = view(p, &dx, &dy);
    /* Fill the inner cross, then the corners row by row. */
    gfx_fill_rect(&v, x + dx + r, y + dy, w - 2 * r, h, fill);
    gfx_fill_rect(&v, x + dx, y + dy + r, r, h - 2 * r, fill);
    gfx_fill_rect(&v, x + dx + w - r, y + dy + r, r, h - 2 * r, fill);
    for (int i = 0; i < r; i++) {
        /* Inset of row i from the top (and bottom) inside the corner. */
        int inset = r - 1 - i;
        int cut = inset > 0 ? (inset + 1) / 2 + (inset > 1 ? 1 : 0) : 0;
        if (cut > r) cut = r;
        gfx_hline(&v, x + dx + cut, y + dy + i, w - 2 * cut, fill);
        gfx_hline(&v, x + dx + cut, y + dy + h - 1 - i, w - 2 * cut, fill);
        if (border != 0xffffffffu) {
            v.pixels[(size_t)(y + dy + i) * v.stride + x + dx + cut] = border;
            v.pixels[(size_t)(y + dy + i) * v.stride + x + dx + w - 1 - cut] = border;
            v.pixels[(size_t)(y + dy + h - 1 - i) * v.stride + x + dx + cut] = border;
            v.pixels[(size_t)(y + dy + h - 1 - i) * v.stride + x + dx + w - 1 - cut] = border;
        }
    }
    if (border != 0xffffffffu) {
        gfx_hline(&v, x + dx + r, y + dy, w - 2 * r, border);
        gfx_hline(&v, x + dx + r, y + dy + h - 1, w - 2 * r, border);
        gfx_vline(&v, x + dx, y + dy + r, h - 2 * r, border);
        gfx_vline(&v, x + dx + w - 1, y + dy + r, h - 2 * r, border);
    }
}

void painter_text(struct painter *p, int x, int y, const char *text, uint32_t color)
{
    int dx, dy;
    struct surface v = view(p, &dx, &dy);
    if (v.width <= 0 || v.height <= 0)
        return;
    gfx_text_font(&v, p->theme->font, x + dx, y + dy, text, color, 0xffffffffu);
}

void painter_text_font(struct painter *p, const struct font *f, int x, int y, const char *text, uint32_t fg, uint32_t bg)
{
    int dx, dy;
    struct surface v = view(p, &dx, &dy);
    if (v.width <= 0 || v.height <= 0)
        return;
    gfx_text_font(&v, f, x + dx, y + dy, text, fg, bg);
}

int painter_text_width(const struct painter *p, const char *text, int n)
{
    return gfx_text_width_font(p->theme->font, text, n);
}

int painter_text_height(const struct painter *p)
{
    return p->theme->font->height;
}

int painter_text_index(const struct painter *p, const char *text, int n, int px)
{
    return gfx_text_index_font(p->theme->font, text, n, px);
}

void painter_focus_ring(struct painter *p, int x, int y, int w, int h)
{
    painter_frame(p, x, y, w, h, p->theme->color[TC_ACCENT]);
}

void painter_blit(struct painter *p, int x, int y, const struct surface *src)
{
    int dx, dy;
    struct surface v = view(p, &dx, &dy);
    if (v.width <= 0 || v.height <= 0)
        return;
    gfx_blit(&v, x + dx, y + dy, src, NULL);
}

void painter_mask(struct painter *p, int x, int y, const uint8_t *mask, int w, int h, uint32_t color)
{
    int dx, dy;
    struct surface v = view(p, &dx, &dy);
    if (v.width <= 0 || v.height <= 0)
        return;
    gfx_blend_mask(&v, x + dx, y + dy, mask, w, h, color);
}

void painter_image(struct painter *p, int x, int y, const struct image *img)
{
    int dx, dy;
    struct surface v = view(p, &dx, &dy);
    for (int j = 0; j < img->h; j++) {
        int py = y + dy + j;
        if (py < 0 || py >= v.height)
            continue;
        uint32_t *row = v.pixels + (size_t)py * v.stride;
        for (int i = 0; i < img->w; i++) {
            int px = x + dx + i;
            if (px < 0 || px >= v.width)
                continue;
            uint32_t c = img->pixels[(size_t)j * img->w + i];
            unsigned a = c >> 24;
            if (!a)
                continue;
            if (a == 255) {
                row[px] = c & 0x00ffffff;
                continue;
            }
            uint32_t d = row[px];
            uint32_t r = ((d >> 16 & 0xff) * (255 - a) + (c >> 16 & 0xff) * a) / 255;
            uint32_t g = ((d >> 8 & 0xff) * (255 - a) + (c >> 8 & 0xff) * a) / 255;
            uint32_t b = ((d & 0xff) * (255 - a) + (c & 0xff) * a) / 255;
            row[px] = r << 16 | g << 8 | b;
        }
    }
}
