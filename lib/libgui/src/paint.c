/* Painter: origin translation, clipping and the logical to device pixel
 * scale over gfx primitives. */
#include <gui/paint.h>
#include <gui/pixel.h>
#include <gui/utf8.h>
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

static int isqrt(int n)
{
    int r = 0;
    while ((r + 1) * (r + 1) <= n)
        r++;
    return r;
}

/* Pixels left out at both ends of row i (from the top or the bottom) of
 * a corner of radius r: the circle's chord at the row's centre. */
static int corner_cut(int r, int i)
{
    int dy = 2 * r - 2 * i - 1;                 /* twice the distance to the centre */
    int dx = isqrt(4 * r * r - dy * dy);        /* twice the half chord */
    int cut = r - (dx + 1) / 2;
    return cut < 0 ? 0 : cut > r ? r : cut;
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
        int cut = corner_cut(r, i);
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
        int cut = corner_cut(r, i);
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

void painter_text_shaped(struct painter *p, int x, int y, const struct gfx_shaped *t, uint32_t fg)
{
    int dx, dy, s = p->scale;
    struct surface v = view(p, &dx, &dy);
    if (v.width <= 0 || v.height <= 0)
        return;
    gfx_shaped_draw(&v, t, x * s + dx, y * s + dy, fg);
}

int painter_text_width(const struct painter *p, const char *text, int n)
{
    return painter_text_width_font(p, NULL, text, n);
}

int painter_text_width_font(const struct painter *p, const struct font *f, const char *text, int n)
{
    int s = p->scale;
    return (gfx_text_width_font_scaled(f ? f : p->theme->font, text, n, s) + s - 1) / s;
}

int painter_text_height(const struct painter *p)
{
    return p->theme->font->height;
}

int painter_wrap(const struct painter *p, const char *text, int w, int *start, int *len, int max)
{
    int lines = 0, at = 0, n = (int)strlen(text);
    while (at < n) {
        /* Find the longest prefix of the line that fits. Where possible,
         * the prefix ends before a space or a newline. */
        int end = at, fit = at, last_space = -1;
        while (end < n && text[end] != '\n') {
            int next = end + 1;
            while (next < n && (text[next] & 0xc0) == 0x80)
                next++;
            if (painter_text_width(p, text + at, next - at) > w)
                break;
            if (text[end] == ' ')
                last_space = end;
            end = next;
            fit = end;
        }
        int line_end, resume;
        if (end >= n || text[end] == '\n' || (text[end] == ' ' && end > at)) {
            /* The end of the text, a newline, or a space right after the
             * part that fits. */
            line_end = end;
            resume = end + 1;
        } else if (last_space > at) {
            line_end = last_space;
            resume = last_space + 1;
        } else {
            /* The word is wider than the line, or the line has no room for
             * even one character. */
            line_end = fit > at ? fit : end + 1;
            while (line_end < n && (text[line_end] & 0xc0) == 0x80)
                line_end++;
            resume = line_end;
        }
        if (lines < max) {
            start[lines] = at;
            len[lines] = line_end - at;
        }
        lines++;
        at = resume;
    }
    return lines;
}

int painter_text_index(const struct painter *p, const char *text, int n, int px)
{
    return painter_text_index_font(p, NULL, text, n, px);
}

int painter_text_index_font(const struct painter *p, const struct font *f, const char *text, int n, int px)
{
    return gfx_text_index_font_scaled(f ? f : p->theme->font, text, n, px * p->scale, p->scale);
}

int painter_mnemonic_strip(const char *text, char *buf, int size)
{
    int j = 0, mnemonic = -1;
    for (int i = 0; text[i] && j + 1 < size; i++) {
        if (text[i] == '&' && text[i + 1]) {
            mnemonic = j;
            continue;
        }
        buf[j++] = text[i];
    }
    buf[j] = '\0';
    return mnemonic;
}

void painter_mnemonic_text(struct painter *p, int x, int y, const char *text, uint32_t color)
{
    char buf[256];
    int mn = painter_mnemonic_strip(text, buf, sizeof buf);
    painter_text(p, x, y, buf, color);
    if (mn < 0)
        return;
    int end = gui_utf8_next_boundary(buf, (int)strlen(buf), mn);
    int x0 = x + painter_text_width(p, buf, mn), x1 = x + painter_text_width(p, buf, end);
    int base = y + painter_text_height(p) - 1;
    painter_line(p, x0, base, x1 - 1, base, color);
}

void painter_focus_ring(struct painter *p, int x, int y, int w, int h)
{
    painter_frame(p, x, y, w, h, p->theme->color[TC_ACCENT]);
}

void painter_avatar(struct painter *p, int x, int y, int size, const char *name, const char *label)
{
    static const uint32_t colors[] = { 0x003c78c8, 0x00c0504d, 0x009bbb59, 0x008064a2, 0x00f79646, 0x004bacc6 };
    unsigned h = 0;
    for (const char *c = name; *c; c++)
        h = h * 31 + (unsigned char)*c;
    uint32_t color = colors[h % (sizeof colors / sizeof colors[0])];
    painter_rounded(p, x, y, size, size, color, color);
    /* The initial is the first character of the label. A lowercase
     * ASCII letter becomes uppercase. */
    char initial[8] = "";
    int at = 0, len = (int)strlen(label);
    if (len) {
        gui_utf8_decode(label, len, &at);
        memcpy(initial, label, (size_t)at);
        initial[at] = '\0';
        if (initial[0] >= 'a' && initial[0] <= 'z')
            initial[0] = (char)(initial[0] - 'a' + 'A');
    }
    int fh = theme_px(p->theme, TM_FONT_PX);
    painter_text(p, x + (size - painter_text_width(p, initial, -1)) / 2, y + (size - fh) / 2 - 1, initial,
                 0x00ffffff);
}

/* The device pixels px0 to px1 of the destination row that an area of
 * width W at X0 covers inside the view of width vw. Returns 0 when it
 * covers none. */
static int clip_span(long X0, long W, int vw, int *px0, int *px1)
{
    *px0 = X0 > 0 ? (int)X0 : 0;
    *px1 = X0 + W < vw ? (int)(X0 + W) : vw;
    return *px1 > *px0;
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
    int X0 = x * s + dx, Y0 = y * s + dy, px0, px1;
    if (!clip_span(X0, (long)src->width * s, v.width, &px0, &px1))
        return;
    for (int j = 0; j < src->height * s; j++) {
        int py = Y0 + j;
        if (py < 0 || py >= v.height)
            continue;
        struct pixel_walk w;
        pixel_walk_init(&w, px0 - X0, 1, s);
        pixel_sample(v.pixels + (size_t)py * v.stride + px0, src->pixels + (size_t)(j / s) * src->stride, 1, px1 - px0,
                     &w);
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
    int X0 = x * s + dx, Y0 = y * s + dy, px0, px1;
    if (!clip_span(X0, (long)w * s, v.width, &px0, &px1))
        return;
    for (int j = 0; j < h * s; j++) {
        int py = Y0 + j;
        if (py < 0 || py >= v.height)
            continue;
        struct pixel_walk wk;
        pixel_walk_init(&wk, px0 - X0, 1, s);
        pixel_mask_sample(v.pixels + (size_t)py * v.stride + px0, mask + (size_t)(j / s) * w, px1 - px0, color, &wk);
    }
}

/* An image of image_lw by image_lh logical pixels: copied one to one
 * when its scale is the painter's, resampled by nearest pixel
 * otherwise (a PNG icon is doubled on a scale 2 output). */
void painter_image(struct painter *p, int x, int y, const struct image *img)
{
    int dx, dy, s = p->scale, is = img->scale > 1 ? img->scale : 1;
    int lw = image_lw(img), lh = image_lh(img);
    struct surface v = view(p, &dx, &dy);
    int X0 = x * s + dx, Y0 = y * s + dy, px0, px1;
    if (v.width <= 0 || v.height <= 0 || !clip_span(X0, (long)lw * s, v.width, &px0, &px1))
        return;
    for (int j = 0; j < lh * s; j++) {
        int py = Y0 + j;
        if (py < 0 || py >= v.height)
            continue;
        struct pixel_walk w;
        pixel_walk_init(&w, px0 - X0, is, s);
        pixel_sample_over(v.pixels + (size_t)py * v.stride + px0, img->pixels + (size_t)(j * is / s) * img->w, 1,
                          px1 - px0, &w);
    }
}

/* Nearest neighbour sampling over the device pixels of the destination
 * that lie inside the clip, so that the cost follows the visible area
 * and not the size of the image. */
void painter_image_scaled(struct painter *p, int x, int y, int w, int h, const struct image *img)
{
    int dx, dy, s = p->scale;
    struct surface v = view(p, &dx, &dy);
    if (v.width <= 0 || v.height <= 0 || w <= 0 || h <= 0)
        return;
    long X0 = (long)x * s + dx, Y0 = (long)y * s + dy, DW = (long)w * s, DH = (long)h * s;
    long py0 = Y0 > 0 ? Y0 : 0, py1 = Y0 + DH < v.height ? Y0 + DH : v.height;
    int px0, px1;
    if (!clip_span(X0, DW, v.width, &px0, &px1))
        return;
    for (long py = py0; py < py1; py++) {
        struct pixel_walk wk;
        pixel_walk_init(&wk, px0 - X0, img->w, DW);
        pixel_sample_over(v.pixels + (size_t)py * v.stride + px0, img->pixels + (size_t)((py - Y0) * img->h / DH) * img->w,
                          1, px1 - px0, &wk);
    }
}
