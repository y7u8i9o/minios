/* pixeltest: the row operations of gui/pixel.h compiled by the cross
 * compiler, SSE2 on x86_64 and NEON on aarch64, against their scalar
 * references, and the rectangle set of gui/gfx.h (the checks of
 * lib/libgui/tests/test_pixel.c, with a sample of the blend values). It
 * then prints the forms that pixel.c chose and the throughput of the
 * vector form, the word form and the reference in millions of pixels per
 * second. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int failures, checks;
#include "../../lib/libgui/tests/test_pixel.c"

#define BENCH_W 1024
#define BENCH_ROWS 1024

static unsigned rate(long us)
{
    return us > 0 ? (unsigned)((long long)BENCH_W * BENCH_ROWS / us) : 0;
}

/* Megapixels per second of fn on BENCH_ROWS rows. */
static unsigned bench_over(void (*fn)(uint32_t *, const uint32_t *, int), uint32_t *to, const uint32_t *from)
{
    long t0 = uptime_us();
    for (int r = 0; r < BENCH_ROWS; r++)
        fn(to, from, BENCH_W);
    return rate(uptime_us() - t0);
}

static unsigned bench_mask(void (*fn)(uint32_t *, const uint8_t *, int, uint32_t), uint32_t *to, const uint8_t *mask)
{
    long t0 = uptime_us();
    for (int r = 0; r < BENCH_ROWS; r++)
        fn(to, mask, BENCH_W, 0x00202020u);
    return rate(uptime_us() - t0);
}

static unsigned bench_darken(void (*fn)(uint32_t *, int, uint32_t), uint32_t *to)
{
    long t0 = uptime_us();
    for (int r = 0; r < BENCH_ROWS; r++)
        fn(to, BENCH_W, 154);
    return rate(uptime_us() - t0);
}

static void ref_over(uint32_t *to, const uint32_t *from, int n) { pixel_over_ref(to, from, n); }
static void ref_mask(uint32_t *to, const uint8_t *m, int n, uint32_t c) { pixel_mask_ref(to, m, n, c); }
static void ref_darken(uint32_t *to, int n, uint32_t k) { pixel_darken_ref(to, n, k); }

static void bench(void)
{
    uint32_t *to = malloc(BENCH_W * 4), *from = malloc(BENCH_W * 4);
    uint8_t *mask = malloc(BENCH_W);
    if (!to || !from || !mask) {
        printf("pixeltest: no memory for the benchmark\n");
        return;
    }
    /* Text and shadows: mostly partial coverage and partial alpha. */
    for (int i = 0; i < BENCH_W; i++) {
        to[i] = 0x00306080u + (uint32_t)i;
        from[i] = (uint32_t)(i * 37 % 256) << 24 | 0x00c08040u;
        mask[i] = (uint8_t)(i * 53);
    }
    printf("pixeltest: forms %s\n", pixel_forms());
    printf("pixeltest: over %u Mpx/s simd, %u word, %u reference\n", bench_over(pixel_over_simd, to, from),
           bench_over(pixel_over_word, to, from), bench_over(ref_over, to, from));
    printf("pixeltest: mask %u Mpx/s simd, %u word, %u reference\n", bench_mask(pixel_mask_simd, to, mask),
           bench_mask(pixel_mask_word, to, mask), bench_mask(ref_mask, to, mask));
    printf("pixeltest: darken %u Mpx/s simd, %u word, %u reference\n", bench_darken(pixel_darken_simd, to),
           bench_darken(pixel_darken_word, to), bench_darken(ref_darken, to));
    free(to);
    free(from);
    free(mask);
}

int main(void)
{
    run_pixel_tests();
    bench();
    printf("pixeltest: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
