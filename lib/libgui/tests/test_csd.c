/* The chrome of the client decorations from the coverage tables and the
 * distance profiles of csd.c (G7 of docs/plan/compositor-performance.md)
 * against the float computation that it replaced, which this file
 * contains as the reference. Every channel may differ by one, because the
 * tables round each part on its own. */
#include "check.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <gui/gfx.h>
#include "../src/csd.h"

/* ---- the reference: the float code of csd.c before G7 ---- */

static float ref_cover(int x, int y, struct rect F, float r)
{
    if (!rect_contains(F, x, y))
        return 0;
    if (r <= 0)
        return 1;
    float px = (float)x + 0.5f, py = (float)y + 0.5f, cx, cy;
    if (px < (float)F.x + r) cx = (float)F.x + r;
    else if (px > (float)(F.x + F.w) - r) cx = (float)(F.x + F.w) - r;
    else return 1;
    if (py < (float)F.y + r) cy = (float)F.y + r;
    else if (py > (float)(F.y + F.h) - r) cy = (float)(F.y + F.h) - r;
    else return 1;
    float dx = px - cx, dy = py - cy;
    float v = r + 0.5f - sqrtf(dx * dx + dy * dy);
    return v < 0 ? 0 : v > 1 ? 1 : v;
}

static float ref_distance(int x, int y, struct rect F, float r)
{
    float px = (float)x + 0.5f, py = (float)y + 0.5f;
    float x0 = (float)F.x + r, y0 = (float)F.y + r, x1 = (float)(F.x + F.w) - r, y1 = (float)(F.y + F.h) - r;
    float dx = px < x0 ? x0 - px : px > x1 ? px - x1 : 0;
    float dy = py < y0 ? y0 - py : py > y1 ? py - y1 : 0;
    float d = sqrtf(dx * dx + dy * dy) - r;
    return d < 0 ? 0 : d;
}

static uint32_t ref_chrome(int x, int y, struct rect F, float R, int S, int active)
{
    float d = ref_distance(x, y, F, R);
    float ring = (float)S;
    float o = d + 0.5f < ring ? 1 : d - 0.5f < ring ? ring - (d - 0.5f) : 0;
    struct rect shadow = { F.x, F.y + CSD_SHADOW_DY * S, F.w, F.h };
    float ds = ref_distance(x, y, shadow, R), reach = (float)(CSD_SHADOW_REACH * S);
    float t = ds >= reach ? 0 : 1.0f - ds / reach;
    float as = (float)(active ? CSD_SHADOW_PEAK : CSD_SHADOW_PEAK_BACKDROP) / 255.0f * t * t;
    float ao = (float)CSD_OUTLINE_ALPHA / 255.0f * o;
    float a = ao + as * (1.0f - ao);
    int ai = (int)(a * 255.0f + 0.5f);
    return (uint32_t)(ai < 0 ? 0 : ai > 255 ? 255 : ai) << 24;
}

static uint32_t ref_over(uint32_t chrome, uint32_t rgb, float cov)
{
    if (cov >= 1)
        return 0xff000000u | (rgb & 0x00ffffff);
    float ac = (float)(chrome >> 24) / 255.0f;
    float a = cov + ac * (1.0f - cov);
    if (a <= 0)
        return 0;
    float k = cov / a;
    uint32_t r = (uint32_t)((float)((rgb >> 16) & 0xff) * k + 0.5f);
    uint32_t g = (uint32_t)((float)((rgb >> 8) & 0xff) * k + 0.5f);
    uint32_t b = (uint32_t)((float)(rgb & 0xff) * k + 0.5f);
    return (uint32_t)(a * 255.0f + 0.5f) << 24 | r << 16 | g << 8 | b;
}

/* ---- comparison ---- */

static int close_pixels(uint32_t a, uint32_t b)
{
    for (int sh = 0; sh < 32; sh += 8) {
        int d = (int)(a >> sh & 0xff) - (int)(b >> sh & 0xff);
        if (d > 1 || d < -1)
            return 0;
    }
    return 1;
}

static void check_scale(int S, int active)
{
    struct csd c = { .enabled = 1, .active = active };
    int w = 120, h = 80, bw, bh;
    csd_buffer_size(&c, w, h, &bw, &bh);
    bw *= S;
    bh *= S;
    struct surface src = { calloc((size_t)bw * bh, 4), bw, bh, bw };
    struct surface dst = { calloc((size_t)bw * bh, 4), bw, bh, bw };
    if (!src.pixels || !dst.pixels) {
        CHECK(0, "no memory");
        return;
    }
    csd_paint(&src, S, &c, w, h);
    struct rect F = rect_scale(csd_frame(&c, w, h), S);
    float R = (float)(CSD_RADIUS * S);
    int bad_chrome = 0, bad_corner = 0, checked_corner = 0;
    for (int y = 0; y < bh; y++)
        for (int x = 0; x < bw; x++) {
            if (rect_contains(F, x, y))
                continue;
            uint32_t want = ref_chrome(x, y, F, R, S, active);
            bad_chrome += !close_pixels(src.pixels[(size_t)y * bw + x], want);
        }
    /* The frame: a colour pattern, then the copy blends its corners. */
    for (int y = F.y; y < F.y + F.h; y++)
        for (int x = F.x; x < F.x + F.w; x++)
            src.pixels[(size_t)y * bw + x] = (uint32_t)(x * 37 + y * 101) & 0x00ffffff;
    csd_copy(&dst, &src, (struct rect){ 0, 0, bw, bh }, S, &c, w, h);
    int Rd = CSD_RADIUS * S;
    for (int y = F.y; y < F.y + F.h; y++)
        for (int x = F.x; x < F.x + F.w; x++) {
            int corner = (y < F.y + Rd || y >= F.y + F.h - Rd) && (x < F.x + Rd || x >= F.x + F.w - Rd);
            if (!corner)
                continue;
            checked_corner++;
            uint32_t from = src.pixels[(size_t)y * bw + x];
            uint32_t want = ref_over(ref_chrome(x, y, F, R, S, active), from, ref_cover(x, y, F, R));
            bad_corner += !close_pixels(dst.pixels[(size_t)y * bw + x], want);
        }
    CHECK(bad_chrome == 0, "scale %d, active %d: %d chrome pixels differ by more than one", S, active, bad_chrome);
    CHECK(checked_corner > 0 && bad_corner == 0, "scale %d, active %d: %d of %d corner pixels differ by more than one",
          S, active, bad_corner, checked_corner);
    free(src.pixels);
    free(dst.pixels);
}

void run_csd_tests(void)
{
    for (int S = 1; S <= 3; S++)
        for (int active = 0; active < 2; active++)
            check_scale(S, active);
}
