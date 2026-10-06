/* The row operations of gui/pixel.h against their scalar references, and
 * the rectangle set against a bitmap of its union. The host test runs
 * this file with the vector code of the host (NEON on an arm64 Mac).
 * user/tests/pixeltest.c includes it in the guest, so that the SSE2 and
 * the NEON code of the cross compilers are compared there. Without
 * PIXEL_EXHAUSTIVE the blend is checked on a sample of the value triples
 * instead of all of them, because TCG runs the guest slowly. */
#include "check.h"
#include <gui/gfx.h>
#include <gui/pixel.h>
#include <stdlib.h>
#include <string.h>
#include "../src/pixel_impl.h"

#define ROW_MAX 68
#define ROW_PAD 8

static uint32_t rng_state = 12345;

static uint32_t rng(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return rng_state >> 1 ^ rng_state << 15;
}

/* A pixel whose alpha is 0, 255 or random, so that the fast paths of the
 * vector code meet mixed rows. */
static uint32_t random_pixel(void)
{
    uint32_t c = rng(), k = rng() % 4;
    if (k == 0)
        return c & 0x00ffffffu;
    if (k == 1)
        return c | 0xff000000u;
    return c;
}

static void random_row(uint32_t *row, int n)
{
    for (int i = 0; i < n; i++)
        row[i] = random_pixel();
}

static void random_mask(uint8_t *mask, int n)
{
    for (int i = 0; i < n; i++) {
        uint32_t k = rng() % 4;
        mask[i] = k == 0 ? 0 : k == 1 ? 255 : (uint8_t)rng();
    }
}

/* The vector form, the word form and the function that chose one of
 * them. */
static void (*const over_forms[3])(uint32_t *, const uint32_t *, int) = { pixel_over_simd, pixel_over_word, pixel_over };
static void (*const mask_forms[3])(uint32_t *, const uint8_t *, int, uint32_t) = { pixel_mask_simd, pixel_mask_word,
                                                                                   pixel_mask };
static void (*const darken_forms[3])(uint32_t *, int, uint32_t) = { pixel_darken_simd, pixel_darken_word, pixel_darken };

/* Every offset from 0 to 7 and every length up to ROW_MAX - 1. Rows of 8
 * pixels with alpha all 0 or all 255 also occur, which the vector code
 * handles without blending. */
static void check_rows(void)
{
    uint32_t a[ROW_MAX + ROW_PAD], b[ROW_MAX + ROW_PAD], src[ROW_MAX + ROW_PAD];
    uint8_t mask[ROW_MAX + ROW_PAD];
    int bad_over = 0, bad_mask = 0, bad_fill = 0, bad_copy = 0, bad_darken = 0;
    for (int round = 0; round < 40; round++)
        for (int off = 0; off < ROW_PAD; off++)
            for (int n = 0; n < ROW_MAX; n++) {
                random_row(a, ROW_MAX + ROW_PAD);
                random_row(src, ROW_MAX + ROW_PAD);
                random_mask(mask, ROW_MAX + ROW_PAD);
                if (round % 5 == 1)
                    for (int i = 0; i < ROW_MAX + ROW_PAD; i++)
                        src[i] |= 0xff000000u;
                if (round % 5 == 2)
                    for (int i = 0; i < ROW_MAX + ROW_PAD; i++)
                        src[i] &= 0x00ffffffu;
                if (round % 5 == 3)
                    memset(mask, round % 2 ? 0xff : 0, sizeof mask);
                uint32_t color = random_pixel(), keep = rng() % 256;

                for (int f = 0; f < 3; f++) {
                    memcpy(b, a, sizeof a);
                    over_forms[f](a + off, src + off, n);
                    pixel_over_ref(b + off, src + off, n);
                    bad_over += memcmp(a, b, sizeof a) != 0;

                    memcpy(b, a, sizeof a);
                    mask_forms[f](a + off, mask + off, n, color);
                    pixel_mask_ref(b + off, mask + off, n, color);
                    bad_mask += memcmp(a, b, sizeof a) != 0;

                    memcpy(b, a, sizeof a);
                    darken_forms[f](a + off, n, keep);
                    pixel_darken_ref(b + off, n, keep);
                    bad_darken += memcmp(a, b, sizeof a) != 0;
                }

                memcpy(b, a, sizeof a);
                pixel_fill(a + off, n, color);
                pixel_fill_ref(b + off, n, color);
                bad_fill += memcmp(a, b, sizeof a) != 0;

                memcpy(b, a, sizeof a);
                pixel_copy_opaque(a + off, src + off, n);
                pixel_copy_opaque_ref(b + off, src + off, n);
                bad_copy += memcmp(a, b, sizeof a) != 0;
            }
    CHECK(bad_over == 0, "a form of pixel_over differs from the reference in %d rows", bad_over);
    CHECK(bad_mask == 0, "a form of pixel_mask differs from the reference in %d rows", bad_mask);
    CHECK(bad_fill == 0, "pixel_fill differs from the reference in %d rows", bad_fill);
    CHECK(bad_copy == 0, "pixel_copy_opaque differs from the reference in %d rows", bad_copy);
    CHECK(bad_darken == 0, "a form of pixel_darken differs from the reference in %d rows", bad_darken);
}

/* pixel_div255 is the division by 255 rounded to the nearest integer. */
static void check_division(void)
{
    int bad = 0;
    for (uint32_t t = 0; t <= 255 * 255; t++)
        bad += pixel_div255(t) != (2 * t + 255) / 510;
    CHECK(bad == 0, "pixel_div255 differs from the rounded division for %d values", bad);
}

/* Both forms of the blend for every triple of alpha, source channel and
 * destination channel, or for a sample of them, against pixel_blend and
 * pixel_shade.
 * Each row of 64 pixels holds 64 destination values of one alpha and one
 * source value in all three colour channels. */
static void check_blend_values(void)
{
    uint32_t row[64], ref[64], src[64];
    uint8_t mask[64];
    int bad = 0, step = 1;
#ifndef PIXEL_EXHAUSTIVE
    step = 7;
#endif
    for (int a = 0; a < 256; a += (a == 0 || a >= 254) ? 1 : step)
        for (int c = 0; c < 256; c += step)
            for (int d0 = 0; d0 < 256; d0 += 64) {
                for (int i = 0; i < 64; i++) {
                    uint32_t d = (uint32_t)(d0 + i);
                    row[i] = 0x5a000000u | d << 16 | d << 8 | d;
                    src[i] = (uint32_t)a << 24 | (uint32_t)c << 16 | (uint32_t)c << 8 | (uint32_t)c;
                    mask[i] = (uint8_t)a;
                }
                memcpy(ref, row, sizeof row);
                for (int f = 0; f < 2; f++) {
                    memcpy(row, ref, sizeof row);
                    over_forms[f](row, src, 64);
                    for (int i = 0; i < 64; i++)
                        if (row[i] != (a ? pixel_blend(ref[i], src[i], (uint32_t)a) : ref[i]))
                            bad++;
                    memcpy(row, ref, sizeof row);
                    mask_forms[f](row, mask, 64, src[0]);
                    for (int i = 0; i < 64; i++)
                        if (row[i] != (a ? pixel_blend(ref[i], src[0], (uint32_t)a) : ref[i]))
                            bad++;
                    memcpy(row, ref, sizeof row);
                    darken_forms[f](row, 64, (uint32_t)a);
                    for (int i = 0; i < 64; i++)
                        if (row[i] != pixel_shade(ref[i], (uint32_t)a))
                            bad++;
                }
            }
    CHECK(bad == 0, "a form of the blend differs from pixel_blend for %d values", bad);
    CHECK(pixel_blend(0x12345678u, 0x00abcdefu, 0) == 0x12345678u, "alpha 0 leaves the destination");
    CHECK(pixel_blend(0x12345678u, 0x00abcdefu, 255) == 0x12abcdefu, "alpha 255 gives the source colour");
}

/* The walk gives floor((start + i) * num / den) for every i. The source
 * is a table whose entry at position p holds p + 4096, so that a sampled
 * value shows the position that the walk read, forwards and backwards. */
static void check_walk(void)
{
    static const long cases[][3] = { { 0, 1, 2 }, { 5, 1, 3 }, { -7, 2, 3 }, { 0, 7, 3 }, { 3, 640, 1920 },
                                     { 11, 1920, 640 }, { -40, 5, 4 }, { 1, 1, 1 } };
    static uint32_t table[8192];
    static uint8_t bytes[8192];
    for (int i = 0; i < 8192; i++) {
        table[i] = (uint32_t)i;
        bytes[i] = (uint8_t)i;
    }
    const uint32_t *origin = table + 4096;
    int bad = 0;
    for (size_t k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        long start = cases[k][0], num = cases[k][1], den = cases[k][2];
        uint32_t fwd[200], back[200];
        uint8_t m[200];
        struct pixel_walk w;
        pixel_walk_init(&w, start, num, den);
        pixel_sample(fwd, origin, 1, 120, &w);
        pixel_sample(fwd + 120, origin, 1, 80, &w);         /* the walk continues */
        pixel_walk_init(&w, start, num, den);
        pixel_sample(back, origin, -1, 200, &w);
        pixel_walk_init(&w, start, num, den);
        pixel_sample_mask(m, bytes + 4096, 200, &w);
        for (long i = 0; i < 200; i++) {
            long p = (start + i) * num, want = p >= 0 ? p / den : -((-p + den - 1) / den);
            bad += fwd[i] != (uint32_t)(4096 + want);
            bad += back[i] != (uint32_t)(4096 - want);
            bad += m[i] != (uint8_t)(4096 + want);
        }
    }
    CHECK(bad == 0, "the walk differs from the division in %d positions", bad);
}

/* Rectangles of the set must be disjoint, cover exactly the union of the
 * added rectangles and number at most RECT_SET_MAX. */
#define GRID 64
static void check_rect_set(void)
{
    int bad_disjoint = 0, bad_cover = 0, bad_count = 0, bad_grow = 0;
    for (int round = 0; round < 400; round++) {
        struct rect_set set;
        rect_set_clear(&set);
        static uint8_t want[GRID][GRID], have[GRID][GRID];
        memset(want, 0, sizeof want);
        int nadd = 1 + (int)(rng() % (round < 200 ? 6 : 60));
        long added_area = 0;
        for (int k = 0; k < nadd; k++) {
            struct rect r = { (int)(rng() % GRID), (int)(rng() % GRID), (int)(rng() % 20), (int)(rng() % 20) };
            r = rect_intersect(r, (struct rect){ 0, 0, GRID, GRID });
            for (int y = r.y; y < r.y + r.h; y++)
                for (int x = r.x; x < r.x + r.w; x++)
                    want[y][x] = 1;
            added_area += (long)r.w * r.h;
            rect_set_add(&set, r);
        }
        bad_count += set.n > RECT_SET_MAX;
        memset(have, 0, sizeof have);
        long area = 0;
        for (int i = 0; i < set.n; i++) {
            struct rect r = set.r[i];
            bad_cover += rect_empty(r);
            area += (long)r.w * r.h;
            for (int y = r.y; y < r.y + r.h; y++)
                for (int x = r.x; x < r.x + r.w; x++) {
                    if (x < 0 || y < 0 || x >= GRID || y >= GRID) {
                        bad_cover++;
                        continue;
                    }
                    bad_disjoint += have[y][x];
                    have[y][x] = 1;
                }
        }
        /* The set may cover more than the union when it merged
         * rectangles. It never misses a pixel of the union. */
        long union_area = 0;
        for (int y = 0; y < GRID; y++)
            for (int x = 0; x < GRID; x++) {
                bad_cover += want[y][x] && !have[y][x];
                union_area += want[y][x];
            }
        /* A set that never collapsed covers at most a quarter more than
         * the union for every merge, which a few small merges keep below
         * twice the union. */
        if (nadd <= 6 && union_area && area > 2 * union_area + 64)
            bad_grow++;
        (void)added_area;
    }
    CHECK(bad_disjoint == 0, "the rectangles of the set overlap in %d pixels", bad_disjoint);
    CHECK(bad_cover == 0, "the set misses %d pixels of the union", bad_cover);
    CHECK(bad_count == 0, "the set exceeds its size %d times", bad_count);
    CHECK(bad_grow == 0, "the set grew far beyond the union %d times", bad_grow);

    struct rect parts[4];
    int n = rect_subtract((struct rect){ 0, 0, 10, 10 }, (struct rect){ 3, 3, 4, 4 }, parts);
    long area = 0;
    for (int i = 0; i < n; i++)
        area += (long)parts[i].w * parts[i].h;
    CHECK(n == 4 && area == 84, "a hole in the middle leaves four parts of 84 pixels: %d parts, %ld", n, area);
    n = rect_subtract((struct rect){ 0, 0, 10, 10 }, (struct rect){ 20, 20, 4, 4 }, parts);
    CHECK(n == 1 && parts[0].w == 10, "a disjoint rectangle leaves the rectangle");
    n = rect_subtract((struct rect){ 2, 2, 4, 4 }, (struct rect){ 0, 0, 10, 10 }, parts);
    CHECK(n == 0, "a covering rectangle leaves nothing");
    struct rect s = rect_scale((struct rect){ 1, 2, 3, 4 }, 2);
    CHECK(s.x == 2 && s.y == 4 && s.w == 6 && s.h == 8, "rect_scale");
}

void run_pixel_tests(void)
{
    check_division();
    check_rows();
    check_blend_values();
    check_walk();
    check_rect_set();
}
