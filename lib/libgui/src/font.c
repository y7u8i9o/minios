/* Bitmap fonts: the built in 8x16 font and .mfnt files. */
#include <gui/gfx.h>
#include <gui/pixel.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <font/font.h>
#include <gui/utf8.h>

static struct font builtin;
static uint32_t builtin_bits[256][GFX_FONT_H];

const struct font *gfx_font_builtin(void)
{
    if (!builtin.height) {
        builtin.height = GFX_FONT_H;
        builtin.ascent = 12;
        for (int c = 0; c < 256; c++) {
            builtin.advance[c] = GFX_FONT_W;
            builtin.width[c] = GFX_FONT_W;
            for (int r = 0; r < GFX_FONT_H; r++)
                builtin_bits[c][r] = (uint32_t)gfx_font8x16[c][r] << 24;
        }
        builtin.bits = &builtin_bits[0][0];
    }
    return &builtin;
}

struct font *gfx_font_load(const char *path)
{
    FILE *fp = fopen(path, "r");
    if (!fp)
        return NULL;
    uint8_t hdr[16];
    if (fread(hdr, 1, sizeof hdr, fp) != sizeof hdr || memcmp(hdr, "MFNT", 4) != 0) {
        fclose(fp);
        errno = EINVAL;
        return NULL;
    }
    int height = hdr[4] | hdr[5] << 8, ascent = hdr[6] | hdr[7] << 8, count = hdr[10] | hdr[11] << 8;
    if (height <= 0 || height > 64 || count != 256) {
        fclose(fp);
        errno = EINVAL;
        return NULL;
    }
    struct font *f = calloc(1, sizeof *f);
    size_t nbits = (size_t)256 * height;
    uint32_t *bits = malloc(nbits * sizeof *bits);
    if (!f || !bits || fread(f->advance, 1, 256, fp) != 256 || fread(f->width, 1, 256, fp) != 256 ||
        fread(bits, sizeof *bits, nbits, fp) != nbits) {
        free(f);
        free(bits);
        fclose(fp);
        errno = EINVAL;
        return NULL;
    }
    fclose(fp);
    f->height = height;
    f->ascent = ascent;
    f->bits = f->owned = bits;
    return f;
}

struct font *gfx_font_open_ttf(const char *path, int px)
{
    struct ofont *of = font_open(path);
    if (!of)
        return NULL;
    struct font *f = calloc(1, sizeof *f);
    if (!f) {
        font_close(of);
        return NULL;
    }
    int ascent, descent, gap;
    font_metrics(of, &ascent, &descent, &gap);
    f->outline = of;
    f->px = px;
    f->ascent = (font_scale(of, ascent, px) + 63) >> 6;
    f->height = f->ascent + ((font_scale(of, -descent, px) + 63) >> 6);
    for (int c = 0; c < 256; c++) {
        int g = font_glyph_index(of, (uint32_t)c);
        int adv = (font_scale(of, font_advance(of, g), px) + 32) >> 6;
        f->advance[c] = (uint8_t)(adv > 255 ? 255 : adv);
        f->width[c] = f->advance[c];
    }
    return f;
}

/* The CJK font is read on first use, because the file has 4 MB.  Its
 * fonts of every size share one outline and are never freed.  The cache is
 * used by the thread that draws, which is the only one in a libgui
 * program. */
#define CJK_PATH "/usr/share/fonts/DroidSansFallbackFull.ttf"
static struct ofont *cjk_outline;
static int cjk_missing;
static struct { int px; struct font *font; } cjk_cache[8];
static int cjk_count;

const struct font *gfx_font_cjk(int px)
{
    for (int i = 0; i < cjk_count; i++)
        if (cjk_cache[i].px == px)
            return cjk_cache[i].font;
    if (cjk_missing || cjk_count == 8)
        return NULL;
    if (!cjk_outline && !(cjk_outline = font_open(CJK_PATH))) {
        cjk_missing = 1;
        return NULL;
    }
    struct font *f = calloc(1, sizeof *f);
    if (!f)
        return NULL;
    int ascent, descent, gap;
    font_metrics(cjk_outline, &ascent, &descent, &gap);
    f->outline = cjk_outline;
    f->px = px;
    f->ascent = (font_scale(cjk_outline, ascent, px) + 63) >> 6;
    f->height = f->ascent + ((font_scale(cjk_outline, -descent, px) + 63) >> 6);
    cjk_cache[cjk_count].px = px;
    cjk_cache[cjk_count].font = f;
    cjk_count++;
    return f;
}

static int cjk_range(uint32_t cp)
{
    return (cp >= 0x2e80 && cp < 0xfe00) || (cp >= 0xff00 && cp < 0xfff0) || (cp >= 0x20000 && cp < 0x40000);
}

void gfx_font_set_fallback(struct font *f, const struct font *fallback)
{
    if (f)
        f->fallback = fallback;
}

void gfx_font_free(struct font *f)
{
    if (f && f != &builtin) {
        if (f->outline)
            font_close(f->outline);
        free(f->owned);
        free(f);
    }
}

void gfx_blend_mask(struct surface *s, int x, int y, const uint8_t *mask, int w, int h, uint32_t fg)
{
    int x0 = x < 0 ? 0 : x, x1 = x + w < s->width ? x + w : s->width;
    if (x1 <= x0)
        return;
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < 0 || py >= s->height)
            continue;
        pixel_mask(s->pixels + (size_t)py * s->stride + x0, mask + (size_t)j * w + (x0 - x), x1 - x0, fg);
    }
}

struct gui_glyph {
    const struct font *font;
    int glyph, byte;
    int32_t x;
};

static int combining(uint32_t cp)
{
    return (cp >= 0x0300 && cp <= 0x036f) || (cp >= 0x1ab0 && cp <= 0x1aff) ||
           (cp >= 0x1dc0 && cp <= 0x1dff) || (cp >= 0x20d0 && cp <= 0x20ff) ||
           (cp >= 0xfe20 && cp <= 0xfe2f);
}

static int shape_outline(const struct font *f, const char *text, int n,
                         struct gui_glyph *out, int max, int32_t *width, int scale)
{
    int len = n < 0 ? (int)strlen(text) : n, count = 0;
    int32_t pen = 0, base_x = 0, base_advance = 0;
    const struct font *prev_font = NULL;
    int prev_glyph = 0;
    for (int at = 0; at < len;) {
        int byte = at;
        uint32_t cp = gui_utf8_decode(text, len, &at);
        const struct font *use = f;
        int glyph = font_glyph_index(f->outline, cp);
        /* The fallback fonts form a chain.  A CJK character that none of
         * them has comes from the CJK font. */
        for (const struct font *fb = f->fallback; !glyph && fb; fb = fb->fallback) {
            int alt = fb->outline ? font_glyph_index(fb->outline, cp) : 0;
            if (alt) {
                use = fb;
                glyph = alt;
            }
        }
        if (!glyph && cjk_range(cp)) {
            const struct font *cjk = gfx_font_cjk(f->px);
            int alt = cjk ? font_glyph_index(cjk->outline, cp) : 0;
            if (alt) {
                use = cjk;
                glyph = alt;
            }
        }
        int32_t advance = font_scale(use->outline, font_advance(use->outline, glyph), use->px * scale);
        int32_t gx = pen;
        if (combining(cp) && count) {
            gx = base_x + (base_advance - advance) / 2;
            advance = 0;
        } else {
            if (prev_glyph && prev_font == use)
                pen += font_scale(use->outline, font_kern(use->outline, prev_glyph, glyph), use->px * scale);
            gx = pen;
            base_x = gx;
            base_advance = advance;
        }
        if (count < max) {
            out[count].font = use;
            out[count].glyph = glyph;
            out[count].byte = byte;
            out[count].x = gx;
        }
        count++;
        pen += advance;
        if (!combining(cp)) {
            prev_font = use;
            prev_glyph = glyph;
        }
    }
    if (width)
        *width = pen;
    return count < max ? count : max;
}

static void text_outline(struct surface *s, const struct font *f, int x, int y, const char *text, uint32_t fg, uint32_t bg,
                         int scale)
{
    int n = (int)strlen(text);
    struct gui_glyph *sh = malloc((size_t)(n + 1) * sizeof *sh);
    if (!sh)
        return;
    int32_t width;
    int count = shape_outline(f, text, n, sh, n, &width, scale);
    if (bg != 0xffffffffu)
        gfx_fill_rect(s, x, y, (width + 63) >> 6, f->height * scale, bg);
    for (int i = 0; i < count; i++) {
        const struct font *use = sh[i].font;
        const struct font_glyph *g = font_render(use->outline, sh[i].glyph, use->px * scale);
        if (!g || !g->bitmap)
            continue;
        int gx = x + ((sh[i].x + 32) >> 6) + g->left;
        int gy = y + f->ascent * scale + g->top;
        gfx_blend_mask(s, gx, gy, g->bitmap, g->width, g->height, fg);
    }
    free(sh);
}

void gfx_text_font_scaled(struct surface *s, const struct font *f, int x, int y, const char *text, uint32_t fg,
                          uint32_t bg, int scale)
{
    if (scale < 1)
        scale = 1;
    if (f->outline) {
        text_outline(s, f, x, y, text, fg, bg, scale);
        return;
    }
    int len = (int)strlen(text);
    for (int at = 0; at < len;) {
        unsigned c = gui_utf8_decode(text, len, &at);
        if (c > 255)
            c = '?';
        const uint32_t *glyph = f->bits + c * f->height;
        int adv = f->advance[c];
        if (bg != 0xffffffffu)
            gfx_fill_rect(s, x, y, adv * scale, f->height * scale, bg);
        int w = f->width[c];
        for (int row = 0; row < f->height; row++) {
            uint32_t bits = glyph[row];
            if (!bits)
                continue;
            for (int col = 0; col < w; col++) {
                if (!(bits & (0x80000000u >> col)))
                    continue;
                if (scale == 1) {
                    int px = x + col, py = y + row;
                    if (px >= 0 && px < s->width && py >= 0 && py < s->height)
                        s->pixels[(size_t)py * s->stride + px] = fg;
                } else {
                    gfx_fill_rect(s, x + col * scale, y + row * scale, scale, scale, fg);
                }
            }
        }
        x += adv * scale;
    }
}

void gfx_text_font(struct surface *s, const struct font *f, int x, int y, const char *text, uint32_t fg, uint32_t bg)
{
    gfx_text_font_scaled(s, f, x, y, text, fg, bg, 1);
}

int gfx_text_width_font_scaled(const struct font *f, const char *text, int n, int scale)
{
    if (scale < 1)
        scale = 1;
    if (f->outline) {
        int32_t width;
        shape_outline(f, text, n, NULL, 0, &width, scale);
        return (width + 32) >> 6;
    }
    int w = 0, len = n < 0 ? (int)strlen(text) : n;
    for (int i = 0; i < len;) {
        unsigned c = gui_utf8_decode(text, len, &i);
        w += f->advance[c <= 255 ? c : '?'] * scale;
    }
    return w;
}

int gfx_text_width_font(const struct font *f, const char *text, int n)
{
    return gfx_text_width_font_scaled(f, text, n, 1);
}

int gfx_text_index_font_scaled(const struct font *f, const char *text, int n, int px, int scale)
{
    if (scale < 1)
        scale = 1;
    if (f->outline) {
        int len = n < 0 ? (int)strlen(text) : n;
        struct gui_glyph *sh = malloc((size_t)(len + 1) * sizeof *sh);
        if (!sh)
            return 0;
        int32_t width;
        int count = shape_outline(f, text, len, sh, len, &width, scale);
        int i, result = len;
        for (i = 0; i < count; i++) {
            int32_t next = i + 1 < count ? sh[i + 1].x : width;
            if (px * 64 < (sh[i].x + next) / 2) {
                result = sh[i].byte;
                break;
            }
        }
        free(sh);
        return result;
    }
    int x = 0, len = n < 0 ? (int)strlen(text) : n;
    for (int i = 0; i < len;) {
        int byte = i;
        unsigned c = gui_utf8_decode(text, len, &i);
        int adv = f->advance[c <= 255 ? c : '?'] * scale;
        if (px < x + adv / 2)
            return byte;
        x += adv;
    }
    return len;
}

int gfx_text_index_font(const struct font *f, const char *text, int n, int px)
{
    return gfx_text_index_font_scaled(f, text, n, px, 1);
}
