/* Painter: origin translation, clipping and the logical to device pixel
 * scale over gfx primitives. */
#include <gui/paint.h>
#include <string.h>

void painter_init_scaled(struct painter *p, struct surface *s, const struct theme *theme, int scale)
{
    memset(p, 0, sizeof *p);
    p->s = s;
    p->theme = theme;
    p->scale = scale > 0 ? scale : 1;
    p->clip = (struct rect){ 0, 0, s->width, s->height };
}

void painter_init(struct painter *p, struct surface *s, const struct theme *theme)
{
    painter_init_scaled(p, s, theme, 1);
}

static int div_floor(int a, int s)
{
    return a >= 0 ? a / s : -((-a + s - 1) / s);
}

void painter_push(struct painter *p, int x, int y, int w, int h)
{
    if (p->depth < PAINTER_DEPTH) {
        p->stack[p->depth] = p->clip;
        p->ostack[p->depth][0] = p->ox;
        p->ostack[p->depth][1] = p->oy;
    }
    p->depth++;
    int s = p->scale;
    struct rect r = { p->ox + x * s, p->oy + y * s, w * s, h * s };
    p->clip = rect_intersect(p->clip, r);
    p->ox += x * s;
    p->oy += y * s;
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

/* A view of the surface limited to the clip, with device coordinates of
 * the local origin translated into it. */
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
    int s = p->scale;
    int x0 = div_floor(p->clip.x - p->ox, s), y0 = div_floor(p->clip.y - p->oy, s);
    int x1 = div_floor(p->clip.x + p->clip.w - p->ox + s - 1, s);
    int y1 = div_floor(p->clip.y + p->clip.h - p->oy + s - 1, s);
    struct rect r = { x0, y0, x1 - x0, y1 - y0 };
    return r;
}

void painter_fill(struct painter *p, int x, int y, int w, int h, uint32_t color)
{
    int dx, dy, s = p->scale;
    struct surface v = view(p, &dx, &dy);
    gfx_fill_rect(&v, x * s + dx, y * s + dy, w * s, h * s, color);
}

void painter_frame(struct painter *p, int x, int y, int w, int h, uint32_t color)
{
    int dx, dy, s = p->scale;
    struct surface v = view(p, &dx, &dy);
    if (s == 1) {
        gfx_rect(&v, x + dx, y + dy, w, h, color);
        return;
    }
    int X = x * s + dx, Y = y * s + dy, W = w * s, H = h * s;
    gfx_fill_rect(&v, X, Y, W, s, color);
    gfx_fill_rect(&v, X, Y + H - s, W, s, color);
    gfx_fill_rect(&v, X, Y, s, H, color);
    gfx_fill_rect(&v, X + W - s, Y, s, H, color);
}

void painter_line(struct painter *p, int x0, int y0, int x1, int y1, uint32_t color)
{
    int dx, dy, s = p->scale;
    struct surface v = view(p, &dx, &dy);
    if (s == 1) {
        gfx_line(&v, x0 + dx, y0 + dy, x1 + dx, y1 + dy, color);
        return;
    }
    int X0 = x0 * s + dx, Y0 = y0 * s + dy, X1 = x1 * s + dx, Y1 = y1 * s + dy;
    if (x0 == x1) {
        int top = Y0 < Y1 ? Y0 : Y1;
        gfx_fill_rect(&v, X0, top, s, (Y0 < Y1 ? Y1 - Y0 : Y0 - Y1) + s, color);
        return;
    }
    if (y0 == y1) {
        int left = X0 < X1 ? X0 : X1;
        gfx_fill_rect(&v, left, Y0, (X0 < X1 ? X1 - X0 : X0 - X1) + s, s, color);
        return;
    }
    /* A diagonal line s pixels thick: s lines shifted along both axes. */
    for (int k = 0; k < s; k++) {
        gfx_line(&v, X0 + k, Y0, X1 + k, Y1, color);
        gfx_line(&v, X0, Y0 + k, X1, Y1 + k, color);
    }
}

/* Filled rounded rectangle without a border, in device pixels. */
static void rounded_fill(struct surface *v, int x, int y, int w, int h, int r, uint32_t fill)
{
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    if (r < 0) r = 0;
    gfx_fill_rect(v, x + r, y, w - 2 * r, h, fill);
    gfx_fill_rect(v, x, y + r, r, h - 2 * r, fill);
    gfx_fill_rect(v, x + w - r, y + r, r, h - 2 * r, fill);
    for (int i = 0; i < r; i++) {
        int inset = r - 1 - i;
        int cut = inset > 0 ? (inset + 1) / 2 + (inset > 1 ? 1 : 0) : 0;
        if (cut > r) cut = r;
        gfx_hline(v, x + cut, y + i, w - 2 * cut, fill);
        gfx_hline(v, x + cut, y + h - 1 - i, w - 2 * cut, fill);
    }
}

void painter_rounded(struct painter *p, int x, int y, int w, int h, uint32_t fill, uint32_t border)
{
    int r = theme_px(p->theme, TM_RADIUS);
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    int dx, dy, s = p->scale;
    struct surface v = view(p, &dx, &dy);
    if (s > 1) {
        /* The border is s pixels: the shape in the border colour, then
         * the inside inset by s. */
        int X = x * s + dx, Y = y * s + dy, W = w * s, H = h * s, R = r * s;
        if (border != 0xffffffffu) {
            rounded_fill(&v, X, Y, W, H, R, border);
            rounded_fill(&v, X + s, Y + s, W - 2 * s, H - 2 * s, R - s, fill);
        } else {
            rounded_fill(&v, X, Y, W, H, R, fill);
        }
        return;
    }
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
    painter_text_font(p, p->theme->font, x, y, text, color, 0xffffffffu);
}

void painter_text_font(struct painter *p, const struct font *f, int x, int y, const char *text, uint32_t fg, uint32_t bg)
{
    int dx, dy, s = p->scale;
    struct surface v = view(p, &dx, &dy);
    if (v.width <= 0 || v.height <= 0)
        return;
    gfx_text_font_scaled(&v, f, x * s + dx, y * s + dy, text, fg, bg, s);
}

int painter_text_width(const struct painter *p, const char *text, int n)
{
    int s = p->scale;
    return (gfx_text_width_font_scaled(p->theme->font, text, n, s) + s - 1) / s;
}

int painter_text_height(const struct painter *p)
{
    return p->theme->font->height;
}

int painter_text_index(const struct painter *p, const char *text, int n, int px)
{
    return gfx_text_index_font_scaled(p->theme->font, text, n, px * p->scale, p->scale);
}

void painter_focus_ring(struct painter *p, int x, int y, int w, int h)
{
    painter_frame(p, x, y, w, h, p->theme->color[TC_ACCENT]);
}

static inline uint32_t blend(uint32_t d, uint32_t c, unsigned a)
{
    uint32_t r = ((d >> 16 & 0xff) * (255 - a) + (c >> 16 & 0xff) * a) / 255;
    uint32_t g = ((d >> 8 & 0xff) * (255 - a) + (c >> 8 & 0xff) * a) / 255;
    uint32_t b = ((d & 0xff) * (255 - a) + (c & 0xff) * a) / 255;
    return r << 16 | g << 8 | b;
}

void painter_blit(struct painter *p, int x, int y, const struct surface *src)
{
    int dx, dy, s = p->scale;
    struct surface v = view(p, &dx, &dy);
    if (v.width <= 0 || v.height <= 0)
        return;
    if (s == 1) {
        gfx_blit(&v, x + dx, y + dy, src, NULL);
        return;
    }
    /* Nearest neighbour enlargement of a logical size source. */
    for (int j = 0; j < src->height * s; j++) {
        int py = y * s + dy + j;
        if (py < 0 || py >= v.height)
            continue;
        const uint32_t *from = src->pixels + (size_t)(j / s) * src->stride;
        uint32_t *row = v.pixels + (size_t)py * v.stride;
        for (int i = 0; i < src->width * s; i++) {
            int px = x * s + dx + i;
            if (px >= 0 && px < v.width)
                row[px] = from[i / s];
        }
    }
}

void painter_mask(struct painter *p, int x, int y, const uint8_t *mask, int w, int h, uint32_t color)
{
    int dx, dy, s = p->scale;
    struct surface v = view(p, &dx, &dy);
    if (v.width <= 0 || v.height <= 0)
        return;
    if (s == 1) {
        gfx_blend_mask(&v, x + dx, y + dy, mask, w, h, color);
        return;
    }
    for (int j = 0; j < h * s; j++) {
        int py = y * s + dy + j;
        if (py < 0 || py >= v.height)
            continue;
        const uint8_t *m = mask + (size_t)(j / s) * w;
        uint32_t *row = v.pixels + (size_t)py * v.stride;
        for (int i = 0; i < w * s; i++) {
            int px = x * s + dx + i;
            unsigned a = m[i / s];
            if (px < 0 || px >= v.width || !a)
                continue;
            row[px] = a == 255 ? color : blend(row[px], color, a);
        }
    }
}

void painter_image(struct painter *p, int x, int y, const struct image *img)
{
    int dx, dy, s = p->scale;
    struct surface v = view(p, &dx, &dy);
    for (int j = 0; j < img->h * s; j++) {
        int py = y * s + dy + j;
        if (py < 0 || py >= v.height)
            continue;
        uint32_t *row = v.pixels + (size_t)py * v.stride;
        const uint32_t *from = img->pixels + (size_t)(j / s) * img->w;
        for (int i = 0; i < img->w * s; i++) {
            int px = x * s + dx + i;
            if (px < 0 || px >= v.width)
                continue;
            uint32_t c = from[i / s];
            unsigned a = c >> 24;
            if (!a)
                continue;
            row[px] = a == 255 ? (c & 0x00ffffff) : blend(row[px], c, a);
        }
    }
}
