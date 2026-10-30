/* The animation clock of X12 (docs/design/animations.md, AN1 of
 * docs/plan/animation-gpu.md).
 *
 * An animation moves the visual parameters of one surface from one value
 * to another along an easing curve. The frame clock of main.c calls
 * anim_advance with the start time of each frame. The advance computes
 * the progress from the time since the start of the animation. A frame
 * that comes late therefore shows a later state and does not slow the
 * animation down. Each step damages the extent of the surface before and
 * after the step. The frame therefore repaints every pixel that the
 * surface leaves or covers.
 *
 * The setting anim_percent scales every duration. 100 is the normal
 * speed, 400 shows the animations four times slower for inspection, and 0
 * disables them: a surface then takes the final values at once.
 *
 * The values are fixed point, like the pixel code. A progress and a scale
 * of 1 are ANIM_ONE. X12 is single threaded. The animations are part of
 * the surfaces and share the state of the scene without a lock. */
#include <stdint.h>
#include <unistd.h>
#include "comp.h"

/* The control points (x1, y1) and (x2, y2) of the cubic Bézier curves in
 * 16.16. The curve starts at (0, 0) and ends at (1, 1), as in CSS. */
static const struct { int32_t x1, y1, x2, y2; } bezier[] = {
    [ANIM_ACCELERATE] = { 26214, 0, ANIM_ONE, ANIM_ONE },        /* 0.4, 0, 1, 1 */
    [ANIM_STANDARD] = { 13107, 0, 0, ANIM_ONE },                 /* 0.2, 0, 0, 1 */
};

static long frames_advanced;

/* One coordinate of the Bézier curve with the inner control values p1
 * and p2 at the parameter t: 3 (1 - t)^2 t p1 + 3 (1 - t) t^2 p2 + t^3.
 * Every product is shifted back to 16.16 at once. The terms therefore fit
 * into 64 bits. */
static int64_t bezier_at(int64_t p1, int64_t p2, int64_t t)
{
    int64_t u = ANIM_ONE - t, tt = t * t >> 16, uu = u * u >> 16;
    return (3 * (uu * t >> 16) * p1 >> 16) + (3 * (u * tt >> 16) * p2 >> 16) + (tt * t >> 16);
}

/* The curve value at the time x. The x coordinate of the curve grows
 * monotonically for control values between 0 and 1. Seventeen bisection
 * steps find the parameter t of x to the last bit of 16.16. */
static int32_t bezier_ease(int curve, int32_t x)
{
    int64_t lo = 0, hi = ANIM_ONE;
    for (int i = 0; i < 17; i++) {
        int64_t mid = (lo + hi) / 2;
        if (bezier_at(bezier[curve].x1, bezier[curve].x2, mid) < x)
            lo = mid;
        else
            hi = mid;
    }
    int64_t y = bezier_at(bezier[curve].y1, bezier[curve].y2, hi);
    return y < 0 ? 0 : y > ANIM_ONE ? ANIM_ONE : (int32_t)y;
}

int32_t anim_ease(enum anim_curve curve, int32_t t)
{
    if (t <= 0)
        return 0;
    if (t >= ANIM_ONE)
        return ANIM_ONE;
    if (curve == ANIM_EASE_OUT_CUBIC) {
        int64_t u = ANIM_ONE - t;
        return (int32_t)(ANIM_ONE - ((u * u >> 16) * u >> 16));
    }
    return bezier_ease(curve, t);
}

/* a + (b - a) * p rounded to the nearest integer. The shift of a
 * negative number rounds towards minus infinity. The rounding is
 * therefore the same in both directions. */
static int32_t mix(int32_t a, int32_t b, int32_t p)
{
    return (int32_t)(((int64_t)a * ANIM_ONE + (int64_t)(b - a) * p + ANIM_ONE / 2) >> 16);
}

struct visual anim_mix(struct visual from, struct visual to, int32_t progress)
{
    struct visual v;
    v.alpha = (uint8_t)mix(from.alpha, to.alpha, progress);
    v.scale = mix(from.scale, to.scale, progress);
    v.dx = mix(from.dx, to.dx, progress);
    v.dy = mix(from.dy, to.dy, progress);
    return v;
}

/* Set the visual parameters of s and damage the extent before and after
 * the change. */
static void set_visual(struct csurface *s, struct visual v)
{
    scene_damage(scene_visual_extent(s));
    s->visual = v;
    scene_damage(scene_visual_extent(s));
}

void anim_start(struct csurface *s, const struct visual *from, struct visual to, int ms, enum anim_curve curve)
{
    if (from)
        set_visual(s, *from);
    long duration = (long)ms * settings.anim_percent * 10;     /* ms * 1000 * percent / 100 */
    if (duration <= 0) {
        s->anim.running = 0;
        set_visual(s, to);
        return;
    }
    s->anim.running = 1;
    s->anim.start_us = uptime_us();
    s->anim.duration_us = duration;
    s->anim.curve = curve;
    s->anim.progress = 0;
    s->anim.from = s->visual;
    s->anim.to = to;
}

void anim_finish(struct csurface *s)
{
    if (!s->anim.running)
        return;
    s->anim.running = 0;
    s->anim.progress = ANIM_ONE;
    set_visual(s, s->anim.to);
}

int anim_running(void)
{
    for (struct csurface *s = surface_first(); s; s = s->next)
        if (s->anim.running)
            return 1;
    return 0;
}

/* One step of the animation of s to the frame time now. */
static void advance(struct csurface *s, long now)
{
    struct anim *a = &s->anim;
    long t = now - a->start_us;
    int32_t linear = t <= 0 ? 0 : t >= a->duration_us ? ANIM_ONE : (int32_t)(t * ANIM_ONE / a->duration_us);
    a->progress = anim_ease(a->curve, linear);
    if (linear == ANIM_ONE) {
        a->running = 0;
        set_visual(s, a->to);
        return;
    }
    set_visual(s, anim_mix(a->from, a->to, a->progress));
}

void anim_advance(long now_us)
{
    int any = 0;
    for (struct csurface *s = surface_first(); s; s = s->next)
        if (s->anim.running) {
            advance(s, now_us);
            any = 1;
        }
    frames_advanced += any;
}

void anim_percent_changed(void)
{
    if (settings.anim_percent)
        return;
    for (struct csurface *s = surface_first(); s; s = s->next)
        anim_finish(s);
}

long anim_count(void)
{
    long n = 0;
    for (struct csurface *s = surface_first(); s; s = s->next)
        n += s->anim.running;
    return n;
}

long anim_frames(void)
{
    return frames_advanced;
}

void anim_reset_frames(void)
{
    frames_advanced = 0;
}
