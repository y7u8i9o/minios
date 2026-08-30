#include <gui/gfx.h>
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

static struct rect clip_to(struct surface *s, int x, int y, int w, int h)
{
    struct rect whole = { 0, 0, s->width, s->height };
    struct rect r = { x, y, w, h };
    return rect_intersect(whole, r);
}

void gfx_fill_rect(struct surface *s, int x, int y, int w, int h, uint32_t color)
{
    struct rect r = clip_to(s, x, y, w, h);
    for (int j = 0; j < r.h; j++) {
        uint32_t *row = s->pixels + (size_t)(r.y + j) * s->stride + r.x;
        for (int i = 0; i < r.w; i++)
            row[i] = color;
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
