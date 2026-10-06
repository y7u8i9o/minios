/* The row operations of gui/pixel.h (docs/design/graphics-performance.md).
 *
 * The blending functions have two forms with the same result:
 *
 * - The vector form works on four pixels. It places two channels of a
 *   pixel into the low bytes of two 16 bit lanes, so that a product of two
 *   channels fits into a lane: red and blue under the mask 0x00ff00ff,
 *   alpha and green shifted down by 8 bits. GCC compiles the vector
 *   extensions to SSE2 on x86_64 and to NEON on aarch64.
 * - The word form works on one pixel in a 64 bit register. The four
 *   channels occupy four 16 bit fields, so that two multiplications blend
 *   a pixel.
 *
 * Both compute d * (255 - a) + c * a + 128 in a lane or field, at most
 * 65153, and divide by 255 as pixel_div255 does. aarch64 uses the vector
 * form. QEMU emulates the SSE2 arithmetic of an x86_64 guest under TCG
 * far more slowly than scalar code, while the same instructions run
 * natively under KVM. On x86_64 the first call of a blending function
 * therefore measures both forms of each function on a short row and
 * retains the faster one. The pixels after the last whole vector go
 * through the scalar references of pixel_impl.h. */
#include <gui/pixel.h>
#include <minios/simd.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "pixel_impl.h"

#define ALPHA_LANES 0xff000000ff000000ull

/* ---- the vector form ---- */

static inline simd_u16x8 splat16(uint16_t v)
{
    return (simd_u16x8){ v, v, v, v, v, v, v, v };
}

static inline simd_u32x4 splat32(uint32_t v)
{
    return (simd_u32x4){ v, v, v, v };
}

static inline simd_u16x8 div255(simd_u16x8 t)
{
    t += splat16(128);
    return (t + (t >> 8)) >> 8;
}

/* d * (255 - a) + c * a, divided by 255, in every lane. */
static inline simd_u16x8 blend16(simd_u16x8 d, simd_u16x8 c, simd_u16x8 a)
{
    return div255(d * (splat16(255) - a) + c * a);
}

/* The colour channels of r with the alpha bytes of d. */
static inline simd_u32x4 with_alpha_of(simd_u32x4 r, simd_u32x4 d)
{
    return (r & splat32(0x00ffffffu)) | (d & splat32(0xff000000u));
}

/* c over d for four pixels with the coverage a, one value of 0 to 255 in
 * each 32 bit lane. */
static inline simd_u32x4 blend4(simd_u32x4 d, simd_u32x4 c, simd_u32x4 a)
{
    simd_u32x4 lo = splat32(0x00ff00ffu);
    simd_u16x8 a2 = (simd_u16x8)(a | a << 16);
    simd_u16x8 rb = blend16((simd_u16x8)(d & lo), (simd_u16x8)(c & lo), a2);
    simd_u16x8 ag = blend16((simd_u16x8)(d >> 8 & lo), (simd_u16x8)(c >> 8 & lo), a2);
    return with_alpha_of((simd_u32x4)rb | (simd_u32x4)ag << 8, d);
}

SIMD_WITHIN_PAGE(512) void pixel_fill(uint32_t *to, int n, uint32_t color)
{
    simd_u32x4 c = splat32(color);
    int i = 0;
    for (; i + 4 <= n; i += 4)
        simd_store_u32x4(to + i, c);
    pixel_fill_ref(to + i, n - i, color);
}

SIMD_WITHIN_PAGE(512) void pixel_copy_opaque(uint32_t *to, const uint32_t *from, int n)
{
    simd_u32x4 rgb = splat32(0x00ffffffu);
    int i = 0;
    for (; i + 4 <= n; i += 4)
        simd_store_u32x4(to + i, simd_load_u32x4(from + i) & rgb);
    pixel_copy_opaque_ref(to + i, from + i, n - i);
}

SIMD_WITHIN_PAGE(1024) void pixel_over_simd(uint32_t *to, const uint32_t *from, int n)
{
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        uint64_t q[2];
        memcpy(q, from + i, 16);
        uint64_t all = q[0] & q[1] & ALPHA_LANES, any = (q[0] | q[1]) & ALPHA_LANES;
        if (!any)
            continue;
        simd_u32x4 d = simd_load_u32x4(to + i), s = simd_load_u32x4(from + i);
        simd_store_u32x4(to + i, all == ALPHA_LANES ? with_alpha_of(s, d) : blend4(d, s, s >> 24));
    }
    pixel_over_ref(to + i, from + i, n - i);
}

SIMD_WITHIN_PAGE(1024) void pixel_mask_simd(uint32_t *to, const uint8_t *mask, int n, uint32_t color)
{
    simd_u32x4 c = splat32(color);
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        uint32_t m4;
        memcpy(&m4, mask + i, 4);
        if (!m4)
            continue;
        simd_u32x4 d = simd_load_u32x4(to + i);
        if (m4 == 0xffffffffu) {
            simd_store_u32x4(to + i, with_alpha_of(c, d));
            continue;
        }
        simd_u32x4 a = { mask[i], mask[i + 1], mask[i + 2], mask[i + 3] };
        simd_store_u32x4(to + i, blend4(d, c, a));
    }
    pixel_mask_ref(to + i, mask + i, n - i, color);
}

SIMD_WITHIN_PAGE(1024) void pixel_darken_simd(uint32_t *to, int n, uint32_t keep)
{
    simd_u32x4 lo = splat32(0x00ff00ffu);
    simd_u16x8 k = splat16((uint16_t)keep);
    int i = 0;
    for (; i + 4 <= n; i += 4) {
        simd_u32x4 d = simd_load_u32x4(to + i);
        simd_u16x8 rb = div255((simd_u16x8)(d & lo) * k), ag = div255((simd_u16x8)(d >> 8 & lo) * k);
        simd_store_u32x4(to + i, with_alpha_of((simd_u32x4)rb | (simd_u32x4)ag << 8, d));
    }
    pixel_darken_ref(to + i, n - i, keep);
}

/* ---- the word form ---- */

#define FIELDS 0x00ff00ff00ff00ffull

/* The word form exists for the cases in which SSE2 is slow, so GCC must
 * not turn its loops into vector code. clang has no such attribute and
 * compiles only the host tests. */
#if __has_attribute(optimize)
#define WORD_FORM __attribute__((optimize("no-tree-vectorize")))
#else
#define WORD_FORM
#endif

/* The four channels of x in the low bytes of four 16 bit fields, in the
 * order blue, red, green, alpha. */
static inline uint64_t spread(uint32_t x)
{
    uint64_t v = x;
    return (v | v << 24) & FIELDS;
}

/* The channels of t, each field divided by 255, as a pixel with the alpha
 * byte of d. */
static inline uint32_t gather(uint64_t t, uint32_t d)
{
    t += 0x0080008000800080ull;
    t = (t + (t >> 8 & FIELDS)) >> 8 & FIELDS;
    return (d & 0xff000000u) | ((uint32_t)(t | t >> 24) & 0x00ffffffu);
}

SIMD_WITHIN_PAGE(512) WORD_FORM void pixel_over_word(uint32_t *to, const uint32_t *from, int n)
{
    for (int i = 0; i < n; i++) {
        uint32_t c = from[i], a = c >> 24, d = to[i];
        if (a == 255)
            to[i] = (d & 0xff000000u) | (c & 0x00ffffffu);
        else if (a)
            to[i] = gather(spread(d) * (255 - a) + spread(c) * a, d);
    }
}

SIMD_WITHIN_PAGE(512) WORD_FORM void pixel_mask_word(uint32_t *to, const uint8_t *mask, int n, uint32_t color)
{
    uint64_t c = spread(color);
    for (int i = 0; i < n; i++) {
        uint32_t a = mask[i], d = to[i];
        if (a == 255)
            to[i] = (d & 0xff000000u) | (color & 0x00ffffffu);
        else if (a)
            to[i] = gather(spread(d) * (255 - a) + c * a, d);
    }
}

SIMD_WITHIN_PAGE(512) WORD_FORM void pixel_darken_word(uint32_t *to, int n, uint32_t keep)
{
    for (int i = 0; i < n; i++)
        to[i] = gather(spread(to[i]) * keep, to[i]);
}

/* ---- the choice of the form ---- */

typedef void (*over_fn)(uint32_t *, const uint32_t *, int);
typedef void (*mask_fn)(uint32_t *, const uint8_t *, int, uint32_t);
typedef void (*darken_fn)(uint32_t *, int, uint32_t);

#if defined(__x86_64__)
/* The chosen forms. Two threads that call a blending function for the
 * first time at once both measure and both store a valid choice, so the
 * pointers need atomic stores only. The probe rows are shared by such
 * threads, which changes only the values of the probe pixels. */
static over_fn chosen_over;
static mask_fn chosen_mask;
static darken_fn chosen_darken;
static const char *chosen_names = "not measured";

#define PROBE_W 256
#define PROBE_ROWS 4

/* Rows with the partial alpha and coverage of text and shadows. */
static uint32_t probe_to[PROBE_W], probe_from[PROBE_W];
static uint8_t probe_mask[PROBE_W];

static void probe_rows(void)
{
    for (int i = 0; i < PROBE_W; i++) {
        probe_to[i] = 0x00306080u + (uint32_t)i;
        probe_from[i] = (uint32_t)(i * 37 % 256) << 24 | 0x00c08040u;
        probe_mask[i] = (uint8_t)(i * 53);
    }
}

/* Each probe returns the microseconds of the fastest of three runs of
 * PROBE_ROWS calls. */
static long probe_over(over_fn fn)
{
    long best = -1;
    for (int run = 0; run < 3; run++) {
        long t0 = uptime_us();
        for (int r = 0; r < PROBE_ROWS; r++)
            fn(probe_to, probe_from, PROBE_W);
        long t = uptime_us() - t0;
        best = best < 0 || t < best ? t : best;
    }
    return best;
}

static long probe_mask_fn(mask_fn fn)
{
    long best = -1;
    for (int run = 0; run < 3; run++) {
        long t0 = uptime_us();
        for (int r = 0; r < PROBE_ROWS; r++)
            fn(probe_to, probe_mask, PROBE_W, 0x00405060u);
        long t = uptime_us() - t0;
        best = best < 0 || t < best ? t : best;
    }
    return best;
}

static long probe_darken(darken_fn fn)
{
    long best = -1;
    for (int run = 0; run < 3; run++) {
        long t0 = uptime_us();
        for (int r = 0; r < PROBE_ROWS; r++)
            fn(probe_to, PROBE_W, 154);
        long t = uptime_us() - t0;
        best = best < 0 || t < best ? t : best;
    }
    return best;
}

static void choose(void)
{
    probe_rows();
    int over_simd = probe_over(pixel_over_simd) <= probe_over(pixel_over_word);
    int mask_simd = probe_mask_fn(pixel_mask_simd) <= probe_mask_fn(pixel_mask_word);
    int darken_simd = probe_darken(pixel_darken_simd) <= probe_darken(pixel_darken_word);
    static const char *const names[8] = {
        "over word, mask word, darken word", "over simd, mask word, darken word",
        "over word, mask simd, darken word", "over simd, mask simd, darken word",
        "over word, mask word, darken simd", "over simd, mask word, darken simd",
        "over word, mask simd, darken simd", "over simd, mask simd, darken simd",
    };
    __atomic_store_n(&chosen_names, names[over_simd | mask_simd << 1 | darken_simd << 2], __ATOMIC_RELAXED);
    __atomic_store_n(&chosen_mask, mask_simd ? pixel_mask_simd : pixel_mask_word, __ATOMIC_RELAXED);
    __atomic_store_n(&chosen_darken, darken_simd ? pixel_darken_simd : pixel_darken_word, __ATOMIC_RELAXED);
    __atomic_store_n(&chosen_over, over_simd ? pixel_over_simd : pixel_over_word, __ATOMIC_RELEASE);
}

static void ensure_chosen(void)
{
    if (!__atomic_load_n(&chosen_over, __ATOMIC_ACQUIRE))
        choose();
}

void pixel_over(uint32_t *to, const uint32_t *from, int n)
{
    ensure_chosen();
    chosen_over(to, from, n);
}

void pixel_mask(uint32_t *to, const uint8_t *mask, int n, uint32_t color)
{
    ensure_chosen();
    chosen_mask(to, mask, n, color);
}

void pixel_darken(uint32_t *to, int n, uint32_t keep)
{
    ensure_chosen();
    chosen_darken(to, n, keep);
}

const char *pixel_forms(void)
{
    ensure_chosen();
    return chosen_names;
}
#else
void pixel_over(uint32_t *to, const uint32_t *from, int n)
{
    pixel_over_simd(to, from, n);
}

void pixel_mask(uint32_t *to, const uint8_t *mask, int n, uint32_t color)
{
    pixel_mask_simd(to, mask, n, color);
}

void pixel_darken(uint32_t *to, int n, uint32_t keep)
{
    pixel_darken_simd(to, n, keep);
}

const char *pixel_forms(void)
{
    return "over simd, mask simd, darken simd";
}
#endif

/* ---- coverage tables ---- */

/* The byte of a coverage c: 255 only for a complete one. */
static uint8_t coverage_byte(float c)
{
    if (c >= 1.0f)
        return 255;
    if (c <= 0.0f)
        return 0;
    int v = (int)(c * 255.0f + 0.5f);
    return (uint8_t)(v > 254 ? 254 : v);
}

/* The cached tables by kind and size. A caller may use several tables at
 * once, so a table is never freed. The sizes in use are few: a handful of
 * radii and buttons at the scales 1 to 4. When the cache is full, a new
 * size gets no table, and the shapes of that size lose their antialiasing. */
#define TABLES 64
static struct {
    int kind, a, b;
    uint8_t *table;
} tables[TABLES];
static int ntables;

static const uint8_t *cached(int kind, int a, int b)
{
    for (int i = 0; i < ntables; i++)
        if (tables[i].kind == kind && tables[i].a == a && tables[i].b == b)
            return tables[i].table;
    return NULL;
}

static const uint8_t *remember(int kind, int a, int b, uint8_t *table)
{
    if (ntables == TABLES) {
        free(table);
        return NULL;
    }
    tables[ntables].kind = kind;
    tables[ntables].a = a;
    tables[ntables].b = b;
    tables[ntables++].table = table;
    return table;
}

const uint8_t *pixel_corner_table(int r)
{
    if (r < 1 || r > 512)
        return NULL;
    const uint8_t *t = cached(0, r, 0);
    if (t || ntables == TABLES)
        return t;
    uint8_t *n = malloc((size_t)r * r);
    if (!n)
        return NULL;
    for (int j = 0; j < r; j++)
        for (int i = 0; i < r; i++) {
            float dx = (float)i + 0.5f - (float)r, dy = (float)j + 0.5f - (float)r;
            n[j * r + i] = coverage_byte((float)r + 0.5f - sqrtf(dx * dx + dy * dy));
        }
    return remember(0, r, 0, n);
}

const uint8_t *pixel_disc_table(int size, int inset)
{
    if (size < 1 || size > 1024)
        return NULL;
    const uint8_t *t = cached(1, size, inset);
    if (t || ntables == TABLES)
        return t;
    uint8_t *n = malloc((size_t)size * size);
    if (!n)
        return NULL;
    float c = (float)size / 2.0f, rad = (float)(size - inset) / 2.0f;
    for (int j = 0; j < size; j++)
        for (int i = 0; i < size; i++) {
            float dx = (float)i + 0.5f - c, dy = (float)j + 0.5f - c;
            n[j * size + i] = coverage_byte(rad + 0.5f - sqrtf(dx * dx + dy * dy));
        }
    return remember(1, size, inset, n);
}

/* ---- sampling and packing ---- */

void pixel_walk_init(struct pixel_walk *w, long start, long num, long den)
{
    long p = start * num;
    long q = p >= 0 ? p / den : -((-p + den - 1) / den);
    w->pos = q;
    w->rem = p - q * den;
    w->inc = num / den;
    w->rinc = num % den;
    w->den = den;
}

SIMD_WITHIN_PAGE(256) void pixel_sample(uint32_t *to, const uint32_t *from, long step, int n, struct pixel_walk *w)
{
    for (int i = 0; i < n; i++) {
        to[i] = from[w->pos * step];
        pixel_walk_next(w);
    }
}

SIMD_WITHIN_PAGE(256) void pixel_sample_mask(uint8_t *to, const uint8_t *from, int n, struct pixel_walk *w)
{
    for (int i = 0; i < n; i++) {
        to[i] = from[w->pos];
        pixel_walk_next(w);
    }
}

/* The sampled pixels pass through a buffer on the stack in pieces of
 * SAMPLE_CHUNK. */
#define SAMPLE_CHUNK 256

void pixel_sample_over(uint32_t *to, const uint32_t *from, long step, int n, struct pixel_walk *w)
{
    uint32_t buf[SAMPLE_CHUNK];
    for (int i = 0; i < n; i += SAMPLE_CHUNK) {
        int k = n - i < SAMPLE_CHUNK ? n - i : SAMPLE_CHUNK;
        pixel_sample(buf, from, step, k, w);
        pixel_over(to + i, buf, k);
    }
}

void pixel_mask_sample(uint32_t *to, const uint8_t *mask, int n, uint32_t color, struct pixel_walk *w)
{
    uint8_t buf[SAMPLE_CHUNK];
    for (int i = 0; i < n; i += SAMPLE_CHUNK) {
        int k = n - i < SAMPLE_CHUNK ? n - i : SAMPLE_CHUNK;
        pixel_sample_mask(buf, mask, k, w);
        pixel_mask(to + i, buf, k, color);
    }
}

void pixel_pack(uint8_t *to, const uint32_t *from, int n, const struct pixel_format *f)
{
    for (int i = 0; i < n; i++) {
        uint32_t c = from[i];
        uint32_t v = ((c >> 16 & 0xff) >> (8 - f->red_size)) << f->red_shift |
                     ((c >> 8 & 0xff) >> (8 - f->green_size)) << f->green_shift |
                     ((c & 0xff) >> (8 - f->blue_size)) << f->blue_shift;
        for (int b = 0; b < f->bytes; b++)
            *to++ = (uint8_t)(v >> (8 * b));
    }
}
