/* Painter: origin translation, clipping and the logical to device pixel
 * scale over gfx primitives. */
#include <gui/paint.h>
#include <gui/pixel.h>
#include <gui/utf8.h>
#include <string.h>
#include <math.h>

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

void painter_push_clip(struct painter *p, int x, int y, int w, int h)
{
    painter_push(p, x, y, w, h);
    p->ox -= x * p->scale;
    p->oy -= y * p->scale;
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

/* The pixel at (x, y) of the view blended with color at coverage a. */
static void blend_at(struct surface *v, int x, int y, uint32_t color, uint32_t a)
{
    if (a == 0 || x < 0 || y < 0 || x >= v->width || y >= v->height)
        return;
    uint32_t *d = v->pixels + (size_t)y * v->stride + x;
    *d = a >= 255 ? color : pixel_blend(*d, color, a);
}

/* A rounded rectangle in device pixels of the view: X, Y, W, H, the
 * radius R and the border width B. The straight parts are filled
 * directly. The corner squares take their coverage from the tables. */
static void round_rect(struct surface *v, int X, int Y, int W, int H, int R, int B, uint32_t fill, uint32_t border)
{
    if (W <= 0 || H <= 0)
        return;
    if (R > W / 2) R = W / 2;
    if (R > H / 2) R = H / 2;
    if (R < 0) R = 0;
    if (border == PAINTER_NONE)
        B = 0;
    int Ri = R - B > 0 ? R - B : 0;
    /* The straight parts: the inside without the corner squares, then the
     * edges of the border. */
    if (fill != PAINTER_NONE) {
        gfx_fill_rect(v, X + B, Y + R, W - 2 * B, H - 2 * R, fill);
        gfx_fill_rect(v, X + R, Y + B, W - 2 * R, R - B, fill);
        gfx_fill_rect(v, X + R, Y + H - R, W - 2 * R, R - B, fill);
    }
    if (B) {
        gfx_fill_rect(v, X + R, Y, W - 2 * R, B, border);
        gfx_fill_rect(v, X + R, Y + H - B, W - 2 * R, B, border);
        gfx_fill_rect(v, X, Y + R, B, H - 2 * R, border);
        gfx_fill_rect(v, X + W - B, Y + R, B, H - 2 * R, border);
    }
    if (!R)
        return;
    const uint8_t *outer = pixel_corner_table(R), *inner = Ri ? pixel_corner_table(Ri) : NULL;
    int corner_x[2] = { X, X + W - R }, corner_y[2] = { Y, Y + H - R };
    for (int cy = 0; cy < 2; cy++)
        for (int cx = 0; cx < 2; cx++)
            for (int y = corner_y[cy]; y < corner_y[cy] + R; y++)
                for (int x = corner_x[cx]; x < corner_x[cx] + R; x++) {
                    uint32_t co = pixel_round_rect_coverage(outer, R, X, Y, W, H, 15, x, y);
                    uint32_t ci = B ? pixel_round_rect_coverage(inner, Ri, X + B, Y + B, W - 2 * B, H - 2 * B, 15, x, y)
                                    : co;
                    if (B && fill == PAINTER_NONE) {
                        blend_at(v, x, y, border, co * (255 - ci) / 255);
                        continue;
                    }
                    if (B)
                        blend_at(v, x, y, border, co);
                    if (fill != PAINTER_NONE)
                        blend_at(v, x, y, fill, ci);
                }
}

void painter_round_rect(struct painter *p, int x, int y, int w, int h, int r, uint32_t fill, uint32_t border)
{
    int dx, dy, s = p->scale;
    struct surface v = view(p, &dx, &dy);
    if (v.width <= 0 || v.height <= 0)
        return;
    round_rect(&v, x * s + dx, y * s + dy, w * s, h * s, r * s, s, fill, border);
}

void painter_rounded(struct painter *p, int x, int y, int w, int h, uint32_t fill, uint32_t border)
{
    painter_round_rect(p, x, y, w, h, theme_px(p->theme, TM_RADIUS), fill, border);
}

void painter_disc(struct painter *p, int x, int y, int size, uint32_t color)
{
    int dx, dy, s = p->scale;
    struct surface v = view(p, &dx, &dy);
    if (v.width <= 0 || v.height <= 0)
        return;
    gfx_disc(&v, x * s + dx, y * s + dy, size * s, 0, color, NULL);
}

void painter_ring(struct painter *p, int x, int y, int size, int width, uint32_t color)
{
    int dx, dy, s = p->scale, S = size * s;
    struct surface v = view(p, &dx, &dy);
    const uint8_t *outer = pixel_disc_table(S, 0), *inner = pixel_disc_table(S, 2 * width * s);
    if (v.width <= 0 || v.height <= 0 || !outer || !inner)
        return;
    int X = x * s + dx, Y = y * s + dy;
    for (int j = 0; j < S; j++)
        for (int i = 0; i < S; i++)
            blend_at(&v, X + i, Y + j, color, (uint32_t)outer[j * S + i] * (255 - inner[j * S + i]) / 255);
}

/* The distance of the point (px, py) from the segment from (ax, ay) to
 * (bx, by). */
static float segment_distance(float px, float py, float ax, float ay, float bx, float by)
{
    float vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    float len2 = vx * vx + vy * vy, t = len2 > 0 ? (wx * vx + wy * vy) / len2 : 0;
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    float ex = wx - t * vx, ey = wy - t * vy;
    return sqrtf(ex * ex + ey * ey);
}

/* Each device pixel near the line takes the coverage width / 2 + 0.5 -
 * distance of its centre, limited to 0 and 1. */
void painter_stroke(struct painter *p, const float *xy, int n, float width, uint32_t color)
{
    int dx, dy, s = p->scale;
    struct surface v = view(p, &dx, &dy);
    if (v.width <= 0 || v.height <= 0 || n < 2)
        return;
    float half = width * (float)s / 2;
    float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
    for (int i = 0; i < n; i++) {
        float px = xy[2 * i] * (float)s + (float)dx, py = xy[2 * i + 1] * (float)s + (float)dy;
        x0 = px < x0 ? px : x0;
        y0 = py < y0 ? py : y0;
        x1 = px > x1 ? px : x1;
        y1 = py > y1 ? py : y1;
    }
    int ix0 = (int)floorf(x0 - half - 1), iy0 = (int)floorf(y0 - half - 1);
    int ix1 = (int)ceilf(x1 + half + 1), iy1 = (int)ceilf(y1 + half + 1);
    for (int y = iy0 < 0 ? 0 : iy0; y <= iy1 && y < v.height; y++)
        for (int x = ix0 < 0 ? 0 : ix0; x <= ix1 && x < v.width; x++) {
            float d = 1e9f;
            for (int i = 0; i + 1 < n; i++) {
                float e = segment_distance((float)x + 0.5f, (float)y + 0.5f, xy[2 * i] * (float)s + (float)dx,
                                           xy[2 * i + 1] * (float)s + (float)dy, xy[2 * i + 2] * (float)s + (float)dx,
                                           xy[2 * i + 3] * (float)s + (float)dy);
                d = e < d ? e : d;
            }
            float c = half + 0.5f - d;
            if (c > 0)
                blend_at(&v, x, y, color, c >= 1 ? 255 : (uint32_t)(c * 255 + 0.5f));
        }
}

void painter_check(struct painter *p, int x, int y, int size, uint32_t color)
{
    float k = (float)size;
    float xy[6] = { x + 0.2f * k, y + 0.52f * k, x + 0.42f * k, y + 0.74f * k, x + 0.8f * k, y + 0.28f * k };
    painter_stroke(p, xy, 3, k / 7 > 1.5f ? k / 7 : 1.5f, color);
}

void painter_chevron(struct painter *p, int x, int y, int size, enum painter_dir dir, uint32_t color)
{
    float k = (float)size, a = 0.3f * k, b = 0.7f * k, c = 0.5f * k, lo = 0.38f * k, hi = 0.62f * k;
    float xy[6];
    switch (dir) {
    case PAINTER_DOWN: xy[0] = a; xy[1] = lo; xy[2] = c; xy[3] = hi; xy[4] = b; xy[5] = lo; break;
    case PAINTER_UP: xy[0] = a; xy[1] = hi; xy[2] = c; xy[3] = lo; xy[4] = b; xy[5] = hi; break;
    case PAINTER_LEFT: xy[0] = hi; xy[1] = a; xy[2] = lo; xy[3] = c; xy[4] = hi; xy[5] = b; break;
    default: xy[0] = lo; xy[1] = a; xy[2] = hi; xy[3] = c; xy[4] = lo; xy[5] = b; break;
    }
    for (int i = 0; i < 6; i += 2) {
        xy[i] += (float)x;
        xy[i + 1] += (float)y;
    }
    painter_stroke(p, xy, 3, k / 9 > 1.5f ? k / 9 : 1.5f, color);
}

void painter_fill_alpha(struct painter *p, int x, int y, int w, int h, uint32_t color, int alpha)
{
    int dx, dy, s = p->scale;
    struct surface v = view(p, &dx, &dy);
    struct rect r = rect_intersect((struct rect){ x * s + dx, y * s + dy, w * s, h * s },
                                   (struct rect){ 0, 0, v.width, v.height });
    for (int j = 0; j < r.h; j++) {
        uint32_t *row = v.pixels + (size_t)(r.y + j) * v.stride + r.x;
        for (int i = 0; i < r.w; i++)
            row[i] = pixel_blend(row[i], color, (uint32_t)alpha);
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
    painter_round_rect(p, x, y, w, h, theme_px(p->theme, TM_RADIUS) - 1, PAINTER_NONE, p->theme->color[TC_ACCENT]);
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
