/* gif.so: the decoder of GIF87a and GIF89a (docs/design/codecs.md, GIF).
 *
 * The decoder reads the blocks of the file in order. A graphic control
 * extension gives the delay, the transparent index and the disposal of
 * the next image. The application extension NETSCAPE2.0 gives the loop
 * count. Each image is LZW data with its own size and position, and with
 * the local or the global colour table. The decoder composes every image
 * on a canvas of the logical screen size. The canvas starts transparent,
 * as in web browsers, and the background colour of the file is ignored.
 * After a frame, disposal 2 clears the area of the frame to transparent,
 * and disposal 3 restores the canvas as it was before the frame. */
#include <codec/codec.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "gif.h"

#define MAX_SIDE 16384
#define MAX_CODES 4096

/* ---- reading ---- */

struct reader {
    const uint8_t *data;
    size_t len, pos;
};

static int byte_at(struct reader *r)
{
    return r->pos < r->len ? r->data[r->pos++] : -1;
}

static int word_at(struct reader *r)
{
    if (r->len - r->pos < 2 || r->pos > r->len)
        return -1;
    int v = r->data[r->pos] | r->data[r->pos + 1] << 8;
    r->pos += 2;
    return v;
}

/* Skips a sequence of data sub-blocks up to its terminator. */
static int skip_blocks(struct reader *r)
{
    for (;;) {
        int n = byte_at(r);
        if (n < 0)
            return -1;
        if (n == 0)
            return 0;
        if ((size_t)n > r->len - r->pos)
            return -1;
        r->pos += (size_t)n;
    }
}

/* ---- LZW ---- */

/* The LZW decoder of one image. It reads the codes from the data
 * sub-blocks and writes the colour indexes in the order of the stream.
 * Returns the number of indexes written; a damaged or truncated stream
 * ends the image early. The reader stops after the sub-block that contains
 * the last code read, and the caller skips the sub-blocks that follow. */
static size_t lzw_run(struct reader *r, int min_size, uint8_t *out, size_t count, int *rest);

static size_t lzw_decode(struct reader *r, int min_size, uint8_t *out, size_t count)
{
    int rest = 0;
    size_t done = lzw_run(r, min_size, out, count, &rest);
    r->pos = (size_t)rest > r->len - r->pos ? r->len : r->pos + (size_t)rest;
    return done;
}

static size_t lzw_run(struct reader *r, int min_size, uint8_t *out, size_t count, int *rest)
{
    static const int unused = -1;
    uint16_t prefix[MAX_CODES];
    uint8_t suffix[MAX_CODES], first[MAX_CODES];
    uint8_t stack[MAX_CODES + 1];
    int clear = 1 << min_size, end = clear + 1;
    int size = min_size + 1, next = clear + 2, old = unused;
    uint32_t bits = 0;
    int nbits = 0, block = 0;
    size_t done = 0;
    for (int i = 0; i < clear; i++) {
        prefix[i] = 0;
        suffix[i] = (uint8_t)i;
        first[i] = (uint8_t)i;
    }
    while (done < count) {
        while (nbits < size) {
            if (block == 0) {
                block = byte_at(r);
                if (block <= 0) {
                    /* The terminator belongs to the caller. */
                    if (block == 0)
                        r->pos--;
                    *rest = 0;
                    return done;
                }
            }
            int b = byte_at(r);
            if (b < 0) {
                *rest = 0;
                return done;
            }
            block--;
            bits |= (uint32_t)b << nbits;
            nbits += 8;
        }
        int code = (int)(bits & ((1u << size) - 1));
        bits >>= size;
        nbits -= size;
        if (code == clear) {
            size = min_size + 1;
            next = clear + 2;
            old = unused;
            continue;
        }
        if (code == end)
            break;
        int depth = 0, c = code;
        if (old == unused) {
            if (code >= clear)
                break;                  /* the first code must be a colour */
            out[done++] = (uint8_t)code;
            old = code;
            continue;
        }
        uint8_t head;
        if (code < next) {
            head = first[code];
        } else if (code == next) {
            /* The code that the decoder defines now: the old string and
             * its own first index. */
            head = first[old];
            stack[depth++] = head;
            c = old;
        } else {
            break;
        }
        while (c >= clear && depth < MAX_CODES) {
            stack[depth++] = suffix[c];
            c = prefix[c];
        }
        stack[depth++] = (uint8_t)c;
        while (depth > 0 && done < count)
            out[done++] = stack[--depth];
        if (next < MAX_CODES) {
            prefix[next] = (uint16_t)old;
            suffix[next] = head;
            first[next] = first[old];
            next++;
            if (next == 1 << size && size < 12)
                size++;
        }
        old = code;
    }
    *rest = block;
    return done;
}

/* ---- the decoder state ---- */

struct gif {
    struct reader r;
    int w, h;
    uint32_t global[256];
    int nglobal;
    uint32_t *canvas;           /* w * h */
    uint32_t *saved;            /* the canvas before a frame with disposal 3 */
    uint8_t *indexes;           /* the indexes of the current image */
    /* The area and the disposal of the last frame. */
    int last_x, last_y, last_w, last_h, last_disposal;
    int loops, frames;
};

static void read_table(struct reader *r, uint32_t *table, int n)
{
    for (int i = 0; i < n; i++) {
        if (r->len - r->pos < 3 || r->pos > r->len) {
            table[i] = 0xff000000u;
            continue;
        }
        const uint8_t *p = r->data + r->pos;
        table[i] = 0xff000000u | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
        r->pos += 3;
    }
}

/* Reads the header and the logical screen. Returns 0 or -EINVAL. */
static int read_header(struct gif *g, const uint8_t *data, size_t len)
{
    g->r.data = data;
    g->r.len = len;
    g->r.pos = 0;
    if (len < 13 || memcmp(data, "GIF", 3) != 0 || (memcmp(data + 3, "87a", 3) != 0 && memcmp(data + 3, "89a", 3) != 0))
        return -EINVAL;
    g->r.pos = 6;
    g->w = word_at(&g->r);
    g->h = word_at(&g->r);
    int packed = byte_at(&g->r);
    byte_at(&g->r);                     /* the background index */
    byte_at(&g->r);                     /* the pixel aspect ratio */
    g->nglobal = 0;
    if (packed & 0x80) {
        g->nglobal = 2 << (packed & 7);
        read_table(&g->r, g->global, g->nglobal);
    }
    return 0;
}

/* Counts the images and reads the loop count without decoding, from the
 * position after the header. */
static void scan(struct gif *g)
{
    struct reader r = g->r;
    g->frames = 0;
    g->loops = 1;                       /* without NETSCAPE2.0 the animation plays once */
    for (;;) {
        int kind = byte_at(&r);
        if (kind == 0x2c) {
            if (r.len - r.pos < 9 || r.pos > r.len)
                return;
            int packed = r.data[r.pos + 8];
            r.pos += 9;
            if (packed & 0x80)
                r.pos += 3u * (2u << (packed & 7));
            if (r.pos >= r.len)
                return;
            r.pos++;                    /* the minimum code size */
            g->frames++;
            if (skip_blocks(&r) < 0)
                return;
        } else if (kind == 0x21) {
            int label = byte_at(&r);
            if (label == 0xff && r.len - r.pos >= 12 && r.data[r.pos] == 11 &&
                (memcmp(r.data + r.pos + 1, "NETSCAPE2.0", 11) == 0 ||
                 memcmp(r.data + r.pos + 1, "ANIMEXTS1.0", 11) == 0)) {
                r.pos += 12;
                if (r.len - r.pos >= 4 && r.data[r.pos] >= 3 && r.data[r.pos + 1] == 1) {
                    int count = r.data[r.pos + 2] | r.data[r.pos + 3] << 8;
                    /* The count is the number of repetitions after the first
                     * play, as web browsers read it. 0 repeats without end. */
                    g->loops = count == 0 ? 0 : count + 1;
                }
            }
            if (label < 0 || skip_blocks(&r) < 0)
                return;
        } else {
            return;                     /* the trailer, or the end of the data */
        }
    }
}

static int gif_open(struct gif *g, const uint8_t *data, size_t len)
{
    memset(g, 0, sizeof *g);
    if (read_header(g, data, len) < 0)
        return -EINVAL;
    scan(g);
    if (g->frames == 0)
        return -EINVAL;
    /* A screen of size 0 takes the size of the first image. */
    if (g->w == 0 || g->h == 0) {
        struct reader r = g->r;
        for (;;) {
            int kind = byte_at(&r);
            if (kind == 0x21) {
                byte_at(&r);
                if (skip_blocks(&r) < 0)
                    return -EINVAL;
                continue;
            }
            if (kind != 0x2c || r.len - r.pos < 8)
                return -EINVAL;
            g->w = r.data[r.pos + 4] | r.data[r.pos + 5] << 8;
            g->h = r.data[r.pos + 6] | r.data[r.pos + 7] << 8;
            break;
        }
    }
    if (g->w <= 0 || g->h <= 0 || g->w > MAX_SIDE || g->h > MAX_SIDE)
        return -EINVAL;
    g->canvas = calloc((size_t)g->w * (size_t)g->h, sizeof *g->canvas);
    if (!g->canvas)
        return -ENOMEM;
    return 0;
}

static void gif_free(struct gif *g)
{
    free(g->canvas);
    free(g->saved);
    free(g->indexes);
    g->canvas = g->saved = NULL;
    g->indexes = NULL;
}

/* Applies the disposal of the last frame to the canvas. */
static void dispose(struct gif *g)
{
    if (g->last_disposal == 2) {
        for (int y = g->last_y; y < g->last_y + g->last_h; y++)
            memset(g->canvas + (size_t)y * g->w + g->last_x, 0, (size_t)g->last_w * sizeof *g->canvas);
    } else if (g->last_disposal == 3 && g->saved) {
        memcpy(g->canvas, g->saved, (size_t)g->w * g->h * sizeof *g->canvas);
    }
    g->last_disposal = 0;
}

/* The row of the image at position i of an interlaced stream. */
static int interlaced_row(int i, int h)
{
    static const int start[4] = { 0, 4, 2, 1 }, step[4] = { 8, 8, 4, 2 };
    for (int pass = 0; pass < 4; pass++) {
        int rows = h > start[pass] ? (h - start[pass] + step[pass] - 1) / step[pass] : 0;
        if (i < rows)
            return start[pass] + i * step[pass];
        i -= rows;
    }
    return -1;
}

/* Decodes the next image onto the canvas. Returns 1 and the delay, 0 at
 * the end of the file, or a negative errno value. */
static int gif_next(struct gif *g, int *delay_ms)
{
    int delay = 0, transparent = -1, disposal = 0;
    for (;;) {
        int kind = byte_at(&g->r);
        if (kind == 0x21) {
            int label = byte_at(&g->r);
            if (label == 0xf9 && g->r.len - g->r.pos >= 5 && g->r.data[g->r.pos] >= 4) {
                const uint8_t *p = g->r.data + g->r.pos + 1;
                disposal = (p[0] >> 2) & 7;
                delay = p[1] | p[2] << 8;
                transparent = (p[0] & 1) ? p[3] : -1;
            }
            if (label < 0 || skip_blocks(&g->r) < 0)
                return 0;
            continue;
        }
        if (kind != 0x2c)
            return 0;                   /* the trailer, an unknown block or the end of the data */
        int x = word_at(&g->r), y = word_at(&g->r), w = word_at(&g->r), h = word_at(&g->r);
        int packed = byte_at(&g->r);
        if (packed < 0 || w <= 0 || h <= 0)
            return 0;
        uint32_t local[256];
        const uint32_t *table = g->global;
        int ntable = g->nglobal;
        if (packed & 0x80) {
            ntable = 2 << (packed & 7);
            read_table(&g->r, local, ntable);
            table = local;
        }
        int min_size = byte_at(&g->r);
        if (min_size < 1 || min_size > 8)
            return 0;
        dispose(g);
        if (disposal == 3) {
            if (!g->saved)
                g->saved = malloc((size_t)g->w * g->h * sizeof *g->saved);
            if (g->saved)
                memcpy(g->saved, g->canvas, (size_t)g->w * g->h * sizeof *g->canvas);
        }
        size_t count = (size_t)w * (size_t)h;
        free(g->indexes);
        g->indexes = malloc(count);
        if (!g->indexes)
            return -ENOMEM;
        size_t got = lzw_decode(&g->r, min_size, g->indexes, count);
        /* The rest of a damaged stream is skipped up to its terminator. */
        if (g->r.pos < g->r.len)
            skip_blocks(&g->r);
        for (size_t i = 0; i < got; i++) {
            int row = (packed & 0x40) ? interlaced_row((int)(i / (size_t)w), h) : (int)(i / (size_t)w);
            int cx = x + (int)(i % (size_t)w), cy = y + row;
            int index = g->indexes[i];
            if (row < 0 || cx >= g->w || cy >= g->h || index == transparent)
                continue;
            g->canvas[(size_t)cy * g->w + cx] = index < ntable ? table[index] : 0xff000000u;
        }
        /* The area of the frame, clipped to the canvas, for its disposal. */
        g->last_x = x < g->w ? x : g->w;
        g->last_y = y < g->h ? y : g->h;
        g->last_w = (x + w < g->w ? x + w : g->w) - g->last_x;
        g->last_h = (y + h < g->h ? y + h : g->h) - g->last_y;
        if (g->last_w < 0)
            g->last_w = 0;
        if (g->last_h < 0)
            g->last_h = 0;
        g->last_disposal = disposal;
        /* Web browsers show a delay of 0 or 10 ms as 100 ms. */
        *delay_ms = delay <= 1 ? 100 : delay * 10;
        return 1;
    }
}

/* ---- the codec functions ---- */

int gif_probe(const uint8_t *data, size_t len)
{
    if (len >= 6 && (memcmp(data, "GIF87a", 6) == 0 || memcmp(data, "GIF89a", 6) == 0))
        return 100;
    return 0;
}

int gif_image_decode(const uint8_t *data, size_t len, const struct codec_image_request *req,
                     struct codec_picture *out)
{
    struct gif g;
    int err = gif_open(&g, data, len);
    if (err < 0) {
        gif_free(&g);
        return err;
    }
    int delay;
    err = gif_next(&g, &delay);
    if (err <= 0) {
        gif_free(&g);
        return err < 0 ? err : -EINVAL;
    }
    out->w = g.w;
    out->h = g.h;
    out->pixels = g.canvas;
    g.canvas = NULL;
    gif_free(&g);
    return 0;
}

int gif_animation_open(const uint8_t *data, size_t len, struct codec_animation_info *info, void **state)
{
    struct gif *g = malloc(sizeof *g);
    if (!g)
        return -ENOMEM;
    int err = gif_open(g, data, len);
    if (err < 0) {
        gif_free(g);
        free(g);
        return err;
    }
    info->w = g->w;
    info->h = g->h;
    info->frames = g->frames;
    info->loops = g->loops;
    *state = g;
    return 0;
}

int gif_animation_next(void *state, uint32_t *pixels, int *delay_ms)
{
    struct gif *g = state;
    int r = gif_next(g, delay_ms);
    if (r > 0)
        memcpy(pixels, g->canvas, (size_t)g->w * g->h * sizeof *pixels);
    return r;
}

void gif_animation_close(void *state)
{
    gif_free(state);
    free(state);
}

static const struct codec gif_codecs[] = {
    {
        .name = "gif",
        .description = "Graphics Interchange Format",
        .kind = CODEC_IMAGE,
        .caps = CODEC_DECODE | CODEC_ENCODE | CODEC_ANIMATED,
        .mime_types = "image/gif",
        .extensions = "gif",
        .probe = gif_probe,
        .image_decode = gif_image_decode,
        .image_encode = gif_image_encode,
        .animation_open = gif_animation_open,
        .animation_next = gif_animation_next,
        .animation_close = gif_animation_close,
        .animation_encode = gif_animation_encode,
    },
};

CODEC_MODULE(gif) = { CODEC_MODULE_ABI, "gif", 1, gif_codecs };
