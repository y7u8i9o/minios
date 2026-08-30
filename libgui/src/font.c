/* Bitmap fonts: the built in 8x16 font and .mfnt files. */
#include <gui/gfx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <font/font.h>
#include <font/font.h>

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
    uint32_t fr = (fg >> 16) & 0xff, fgc = (fg >> 8) & 0xff, fb = fg & 0xff;
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < 0 || py >= s->height)
            continue;
        const uint8_t *m = mask + (size_t)j * w;
        uint32_t *row = s->pixels + (size_t)py * s->stride;
        for (int i = 0; i < w; i++) {
            int px = x + i;
            unsigned a = m[i];
            if (px < 0 || px >= s->width || !a)
                continue;
            if (a == 255) {
                row[px] = fg;
                continue;
            }
            /* a in 0..255 scaled to 0..256 so the blend is a shift. */
            uint32_t d = row[px], ia = 256 - (a + (a >> 7));
            a += a >> 7;
            uint32_t r = ((d >> 16 & 0xff) * ia + fr * a) >> 8;
            uint32_t g = ((d >> 8 & 0xff) * ia + fgc * a) >> 8;
            uint32_t b = ((d & 0xff) * ia + fb * a) >> 8;
            row[px] = r << 16 | g << 8 | b;
        }
    }
}

static void text_outline(struct surface *s, const struct font *f, int x, int y, const char *text, uint32_t fg, uint32_t bg)
{
    int n = (int)strlen(text);
    struct font_shaped *sh = malloc((size_t)(n + 1) * sizeof *sh);
    if (!sh)
        return;
    int32_t width;
    int count = font_shape(f->outline, text, n, f->px, sh, n, &width);
    if (bg != 0xffffffffu)
        gfx_fill_rect(s, x, y, (width + 63) >> 6, f->height, bg);
    for (int i = 0; i < count; i++) {
        const struct font_glyph *g = font_render(f->outline, sh[i].glyph, f->px);
        if (!g || !g->bitmap)
            continue;
        int gx = x + ((sh[i].x + 32) >> 6) + g->left;
        int gy = y + f->ascent + g->top;
        gfx_blend_mask(s, gx, gy, g->bitmap, g->width, g->height, fg);
    }
    free(sh);
}

void gfx_text_font(struct surface *s, const struct font *f, int x, int y, const char *text, uint32_t fg, uint32_t bg)
{
    if (f->outline) {
        text_outline(s, f, x, y, text, fg, bg);
        return;
    }
    for (; *text; text++) {
        unsigned c = (unsigned char)*text;
        const uint32_t *glyph = f->bits + c * f->height;
        int adv = f->advance[c];
        if (bg != 0xffffffffu)
            gfx_fill_rect(s, x, y, adv, f->height, bg);
        int w = f->width[c];
        for (int row = 0; row < f->height; row++) {
            int py = y + row;
            if (py < 0 || py >= s->height)
                continue;
            uint32_t bits = glyph[row];
            if (!bits)
                continue;
            for (int col = 0; col < w; col++) {
                int px = x + col;
                if (px >= 0 && px < s->width && (bits & (0x80000000u >> col)))
                    s->pixels[(size_t)py * s->stride + px] = fg;
            }
        }
        x += adv;
    }
}

int gfx_text_width_font(const struct font *f, const char *text, int n)
{
    if (f->outline) {
        int32_t width;
        font_shape(f->outline, text, n, f->px, NULL, 0, &width);
        return (width + 32) >> 6;
    }
    int w = 0;
    for (int i = 0; text[i] && (n < 0 || i < n); i++)
        w += f->advance[(unsigned char)text[i]];
    return w;
}

int gfx_text_index_font(const struct font *f, const char *text, int n, int px)
{
    if (f->outline) {
        int len = n < 0 ? (int)strlen(text) : n;
        struct font_shaped *sh = malloc((size_t)(len + 1) * sizeof *sh);
        if (!sh)
            return 0;
        int32_t width;
        int count = font_shape(f->outline, text, len, f->px, sh, len, &width);
        int i;
        for (i = 0; i < count; i++) {
            int32_t next = i + 1 < count ? sh[i + 1].x : width;
            if (px * 64 < (sh[i].x + next) / 2)
                break;
        }
        free(sh);
        return i;
    }
    int x = 0, i;
    for (i = 0; text[i] && (n < 0 || i < n); i++) {
        int adv = f->advance[(unsigned char)text[i]];
        if (px < x + adv / 2)
            return i;
        x += adv;
    }
    return i;
}
