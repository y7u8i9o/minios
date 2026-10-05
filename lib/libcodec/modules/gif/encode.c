/* The encoder of gif.so (docs/design/codecs.md, GIF).
 *
 * Every frame covers the whole canvas and has a local colour table. A
 * pixel with an alpha below 128 becomes the transparent index, and the
 * other pixels are opaque. A frame with at most 256 colours, the
 * transparent index included, receives exactly its colours. A frame with
 * more colours receives a table chosen by median cut over a histogram of
 * 15 bit colours, and each pixel takes the nearest colour of the table.
 * The encoder does not dither. An animation receives the application
 * extension NETSCAPE2.0 with its loop count, and every frame a graphic
 * control extension with its delay. Frames with transparent pixels use
 * disposal 2, so that the next frame starts on a transparent canvas. */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "gif.h"

#define MAX_CODES 4096
#define HIST 32768                      /* 15 bit colours */

/* ---- output ---- */

struct out {
    uint8_t *data;
    size_t len, cap;
    int failed;
};

static void put(struct out *o, const void *p, size_t n)
{
    if (o->failed)
        return;
    if (o->len + n > o->cap) {
        size_t cap = o->cap ? o->cap * 2 : 4096;
        while (cap < o->len + n)
            cap *= 2;
        uint8_t *d = realloc(o->data, cap);
        if (!d) {
            o->failed = 1;
            return;
        }
        o->data = d;
        o->cap = cap;
    }
    memcpy(o->data + o->len, p, n);
    o->len += n;
}

static void put_byte(struct out *o, int b)
{
    uint8_t v = (uint8_t)b;
    put(o, &v, 1);
}

static void put_word(struct out *o, int w)
{
    put_byte(o, w & 0xff);
    put_byte(o, w >> 8);
}

/* ---- palettes ---- */

struct palette {
    uint32_t color[256];                /* 0x00rrggbb */
    int count;
    int transparent;                    /* the transparent index, or -1 */
};

static int opaque(uint32_t p) { return (p >> 24) >= 128; }
static int key15(uint32_t p) { return (int)((p >> 9 & 0x7c00) | (p >> 6 & 0x3e0) | (p >> 3 & 0x1f)); }

/* The exact colours of a frame when they fit into the table. Returns 1,
 * or 0 when the frame has too many colours. */
static int exact_palette(const uint32_t *pixels, size_t n, struct palette *pal, int transparent)
{
    enum { SLOTS = 1024 };
    uint32_t keys[SLOTS];
    int used[SLOTS];
    memset(used, 0, sizeof used);
    int limit = transparent ? 255 : 256;
    pal->count = 0;
    for (size_t i = 0; i < n; i++) {
        if (!opaque(pixels[i]))
            continue;
        uint32_t c = pixels[i] & 0xffffff;
        unsigned h = (c * 2654435761u) >> 22;
        while (used[h] && keys[h] != c)
            h = (h + 1) & (SLOTS - 1);
        if (used[h])
            continue;
        if (pal->count == limit)
            return 0;
        used[h] = 1;
        keys[h] = c;
        pal->color[pal->count++] = c;
    }
    return 1;
}

/* A box of the median cut: a range of entries of the sorted list of
 * occupied histogram cells. */
struct box {
    int first, count;
    long pixels;
};

static int cell_axis(int key, int axis) { return key >> (10 - 5 * axis) & 31; }

/* Sorts n cells by one axis with a counting sort over its 32 values. tmp
 * has room for n cells. */
static void sort_cells(int *cells, int n, int axis, int *tmp)
{
    int start[33] = { 0 };
    for (int i = 0; i < n; i++)
        start[cell_axis(cells[i], axis) + 1]++;
    for (int v = 0; v < 32; v++)
        start[v + 1] += start[v];
    for (int i = 0; i < n; i++)
        tmp[start[cell_axis(cells[i], axis)]++] = cells[i];
    memcpy(cells, tmp, (size_t)n * sizeof *cells);
}

/* Median cut over the 15 bit histogram of the opaque pixels. Returns 0
 * or -ENOMEM. */
static int cut_palette(const uint32_t *pixels, size_t n, struct palette *pal, int transparent)
{
    int err = 0;
    long *hist = calloc(HIST, sizeof *hist);
    long (*sums)[3] = calloc(HIST, sizeof *sums);
    int *cells = malloc(HIST * sizeof *cells), *tmp = malloc(HIST * sizeof *tmp);
    int limit = transparent ? 255 : 256, ncells = 0;
    pal->count = 0;
    if (!hist || !sums || !cells || !tmp) {
        err = -ENOMEM;
        goto done;
    }
    for (size_t i = 0; i < n; i++) {
        if (!opaque(pixels[i]))
            continue;
        int k = key15(pixels[i]);
        if (hist[k]++ == 0)
            cells[ncells++] = k;
        sums[k][0] += pixels[i] >> 16 & 0xff;
        sums[k][1] += pixels[i] >> 8 & 0xff;
        sums[k][2] += pixels[i] & 0xff;
    }
    struct box boxes[256];
    int nboxes = 0;
    if (ncells > 0) {
        boxes[0] = (struct box){ 0, ncells, (long)n };
        nboxes = 1;
    }
    while (nboxes < limit) {
        /* The box with the largest range along one axis, weighted by its
         * pixels, is split at the median of its pixels. */
        int best = -1, best_axis = 0;
        long best_score = 0;
        for (int b = 0; b < nboxes; b++) {
            if (boxes[b].count < 2)
                continue;
            for (int axis = 0; axis < 3; axis++) {
                int lo = 31, hi = 0;
                for (int i = 0; i < boxes[b].count; i++) {
                    int v = cell_axis(cells[boxes[b].first + i], axis);
                    lo = v < lo ? v : lo;
                    hi = v > hi ? v : hi;
                }
                long score = (long)(hi - lo) * boxes[b].pixels;
                if (hi > lo && score > best_score) {
                    best_score = score;
                    best = b;
                    best_axis = axis;
                }
            }
        }
        if (best < 0)
            break;
        struct box *bx = &boxes[best];
        sort_cells(cells + bx->first, bx->count, best_axis, tmp);
        long total = 0, half = 0;
        for (int i = 0; i < bx->count; i++)
            total += hist[cells[bx->first + i]];
        int split = 1;
        for (int i = 0; i < bx->count - 1; i++) {
            half += hist[cells[bx->first + i]];
            split = i + 1;
            if (half * 2 >= total)
                break;
        }
        /* The split falls between two different values of the axis: the
         * first such boundary at or after the median, or else the last one
         * before it. The range of the box guarantees one boundary. */
#define VALUE(i) cell_axis(cells[bx->first + (i)], best_axis)
        int t = split;
        while (t < bx->count && VALUE(t) == VALUE(t - 1))
            t++;
        if (t == bx->count) {
            t = split;
            while (t > 1 && VALUE(t) == VALUE(t - 1))
                t--;
        }
#undef VALUE
        split = t;
        long left = 0;
        for (int i = 0; i < split; i++)
            left += hist[cells[bx->first + i]];
        boxes[nboxes] = (struct box){ bx->first + split, bx->count - split, bx->pixels - left };
        bx->count = split;
        bx->pixels = left;
        nboxes++;
    }
    for (int b = 0; b < nboxes; b++) {
        long r = 0, g = 0, bl = 0, count = 0;
        for (int i = 0; i < boxes[b].count; i++) {
            int k = cells[boxes[b].first + i];
            r += sums[k][0];
            g += sums[k][1];
            bl += sums[k][2];
            count += hist[k];
        }
        if (count == 0)
            continue;
        pal->color[pal->count++] = (uint32_t)((r + count / 2) / count) << 16 |
                                   (uint32_t)((g + count / 2) / count) << 8 | (uint32_t)((bl + count / 2) / count);
    }
done:
    free(hist);
    free(sums);
    free(cells);
    free(tmp);
    return err;
}

static int nearest(const struct palette *pal, uint32_t c)
{
    int best = 0;
    long best_d = -1;
    int r = c >> 16 & 0xff, g = c >> 8 & 0xff, b = c & 0xff;
    for (int i = 0; i < pal->count; i++) {
        if (i == pal->transparent)
            continue;
        int dr = r - (int)(pal->color[i] >> 16 & 0xff), dg = g - (int)(pal->color[i] >> 8 & 0xff),
            db = b - (int)(pal->color[i] & 0xff);
        long d = 2L * dr * dr + 4L * dg * dg + 3L * db * db;
        if (best_d < 0 || d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

/* Builds the table of a frame and maps its pixels to indexes. Returns 0
 * or -ENOMEM. */
static int map_frame(const uint32_t *pixels, size_t n, struct palette *pal, uint8_t *indexes)
{
    int transparent = 0;
    for (size_t i = 0; i < n && !transparent; i++)
        transparent = !opaque(pixels[i]);
    int exact = exact_palette(pixels, n, pal, transparent);
    if (!exact && cut_palette(pixels, n, pal, transparent) < 0)
        return -ENOMEM;
    pal->transparent = -1;
    if (transparent || pal->count == 0) {
        pal->transparent = pal->count;
        pal->color[pal->count++] = 0;
    }
    if (exact) {
        /* The exact table: a small hash of the colours to their index. */
        enum { SLOTS = 1024 };
        uint32_t keys[SLOTS];
        int index[SLOTS];
        memset(index, -1, sizeof index);
        for (int i = 0; i < pal->count; i++) {
            if (i == pal->transparent)
                continue;
            unsigned h = (pal->color[i] * 2654435761u) >> 22;
            while (index[h] >= 0)
                h = (h + 1) & (SLOTS - 1);
            keys[h] = pal->color[i];
            index[h] = i;
        }
        for (size_t i = 0; i < n; i++) {
            if (!opaque(pixels[i])) {
                indexes[i] = (uint8_t)pal->transparent;
                continue;
            }
            uint32_t c = pixels[i] & 0xffffff;
            unsigned h = (c * 2654435761u) >> 22;
            while (index[h] < 0 || keys[h] != c)
                h = (h + 1) & (SLOTS - 1);
            indexes[i] = (uint8_t)index[h];
        }
        return 0;
    }
    /* The nearest colour of every 15 bit cell, computed when the cell is
     * first used. */
    int16_t *cache = malloc(HIST * sizeof *cache);
    if (!cache)
        return -ENOMEM;
    memset(cache, -1, HIST * sizeof *cache);
    for (size_t i = 0; i < n; i++) {
        if (!opaque(pixels[i])) {
            indexes[i] = (uint8_t)pal->transparent;
            continue;
        }
        int k = key15(pixels[i]);
        if (cache[k] < 0) {
            /* The centre of the cell stands for its colours. */
            uint32_t centre = (uint32_t)((k >> 10 & 31) << 3 | 4) << 16 | (uint32_t)((k >> 5 & 31) << 3 | 4) << 8 |
                              (uint32_t)((k & 31) << 3 | 4);
            cache[k] = (int16_t)nearest(pal, centre);
        }
        indexes[i] = (uint8_t)cache[k];
    }
    free(cache);
    return 0;
}

/* ---- LZW ---- */

struct bits {
    struct out *o;
    uint8_t block[255];
    int fill;
    uint32_t acc;
    int nacc;
};

static void emit(struct bits *b, int code, int size)
{
    b->acc |= (uint32_t)code << b->nacc;
    b->nacc += size;
    while (b->nacc >= 8) {
        b->block[b->fill++] = (uint8_t)b->acc;
        b->acc >>= 8;
        b->nacc -= 8;
        if (b->fill == 255) {
            put_byte(b->o, 255);
            put(b->o, b->block, 255);
            b->fill = 0;
        }
    }
}

static void finish(struct bits *b)
{
    if (b->nacc > 0) {
        b->block[b->fill++] = (uint8_t)b->acc;
        b->acc = 0;
        b->nacc = 0;
    }
    if (b->fill > 0) {
        put_byte(b->o, b->fill);
        put(b->o, b->block, (size_t)b->fill);
    }
    put_byte(b->o, 0);
}

/* The LZW data of one image: a hash table maps a pair of a prefix code
 * and an index to the code of the string. */
static int lzw_encode(struct out *o, const uint8_t *indexes, size_t n, int min_size)
{
    enum { SLOTS = 8192 };
    uint32_t *keys = malloc(SLOTS * sizeof *keys);
    int16_t *codes = malloc(SLOTS * sizeof *codes);
    if (!keys || !codes) {
        free(keys);
        free(codes);
        return -ENOMEM;
    }
    struct bits b = { .o = o };
    int clear = 1 << min_size, end = clear + 1;
    int size = min_size + 1, next = clear + 2;
    memset(codes, -1, SLOTS * sizeof *codes);
    put_byte(o, min_size);
    emit(&b, clear, size);
    int prefix = n ? indexes[0] : -1;
    for (size_t i = 1; i < n; i++) {
        int c = indexes[i];
        uint32_t key = (uint32_t)prefix << 8 | (uint32_t)c;
        unsigned h = (key * 2654435761u) >> 19;
        while (codes[h] >= 0 && keys[h] != key)
            h = (h + 1) & (SLOTS - 1);
        if (codes[h] >= 0) {
            prefix = codes[h];
            continue;
        }
        emit(&b, prefix, size);
        if (next < MAX_CODES) {
            keys[h] = key;
            codes[h] = (int16_t)next;
            /* The decoder raises the code size after it defines the
             * code 2^size, one code later than the encoder defines it. */
            if (next == 1 << size && size < 12)
                size++;
            next++;
        } else {
            emit(&b, clear, size);
            size = min_size + 1;
            next = clear + 2;
            memset(codes, -1, SLOTS * sizeof *codes);
        }
        prefix = c;
    }
    if (prefix >= 0)
        emit(&b, prefix, size);
    emit(&b, end, size);
    finish(&b);
    free(keys);
    free(codes);
    return 0;
}

/* ---- the file ---- */

static int frame_bits(int count)
{
    int bits = 1;
    while ((1 << bits) < count)
        bits++;
    return bits;
}

static int write_frame(struct out *o, int w, int h, const uint32_t *pixels, int delay_ms, int animated)
{
    size_t n = (size_t)w * (size_t)h;
    uint8_t *indexes = malloc(n);
    if (!indexes)
        return -ENOMEM;
    struct palette pal;
    int err = map_frame(pixels, n, &pal, indexes);
    if (err < 0) {
        free(indexes);
        return err;
    }
    if (animated || pal.transparent >= 0) {
        int disposal = animated ? (pal.transparent >= 0 ? 2 : 1) : 0;
        int cs = (delay_ms + 5) / 10;
        put_byte(o, 0x21);
        put_byte(o, 0xf9);
        put_byte(o, 4);
        put_byte(o, disposal << 2 | (pal.transparent >= 0));
        put_word(o, cs > 0xffff ? 0xffff : cs);
        put_byte(o, pal.transparent >= 0 ? pal.transparent : 0);
        put_byte(o, 0);
    }
    int bits = frame_bits(pal.count);
    put_byte(o, 0x2c);
    put_word(o, 0);
    put_word(o, 0);
    put_word(o, w);
    put_word(o, h);
    put_byte(o, 0x80 | (bits - 1));
    for (int i = 0; i < 1 << bits; i++) {
        uint32_t c = i < pal.count ? pal.color[i] : 0;
        uint8_t rgb[3] = { (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
        put(o, rgb, 3);
    }
    err = lzw_encode(o, indexes, n, bits < 2 ? 2 : bits);
    free(indexes);
    return err;
}

static long write_file(int w, int h, const struct codec_frame *frames, int count, int loops, uint8_t **data)
{
    if (w <= 0 || h <= 0 || w > 0xffff || h > 0xffff)
        return -EINVAL;
    struct out o = { 0 };
    int animated = count > 1 || loops != 1;
    put(&o, "GIF89a", 6);
    put_word(&o, w);
    put_word(&o, h);
    put_byte(&o, 0x70);                 /* no global table, 8 bits of colour resolution */
    put_byte(&o, 0);
    put_byte(&o, 0);
    if (animated && loops != 1) {
        /* The count is the number of repetitions after the first play. */
        put_byte(&o, 0x21);
        put_byte(&o, 0xff);
        put_byte(&o, 11);
        put(&o, "NETSCAPE2.0", 11);
        put_byte(&o, 3);
        put_byte(&o, 1);
        put_word(&o, loops == 0 ? 0 : (loops - 1 > 0xffff ? 0xffff : loops - 1));
        put_byte(&o, 0);
    }
    int err = 0;
    for (int i = 0; i < count && err == 0; i++)
        err = write_frame(&o, w, h, frames[i].pixels, frames[i].delay_ms, animated);
    put_byte(&o, 0x3b);
    if (err == 0 && o.failed)
        err = -ENOMEM;
    if (err < 0) {
        free(o.data);
        return err;
    }
    *data = o.data;
    return (long)o.len;
}

long gif_image_encode(const struct codec_picture *pic, uint8_t **data)
{
    struct codec_frame f = { pic->pixels, 0 };
    return write_file(pic->w, pic->h, &f, 1, 1, data);
}

long gif_animation_encode(const struct codec_animation_info *info, const struct codec_frame *frames, int count,
                          uint8_t **data)
{
    return write_file(info->w, info->h, frames, count, info->loops, data);
}
