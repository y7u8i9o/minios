/* The debug views of x12settings (docs/design/x12settings.md): the
 * outline of a highlighted surface, the flash of damaged rectangles, the
 * tint of opaque regions and a frame counter. The views draw over the
 * composed rectangles, before the cursor. The damage that a view adds
 * for its own drawing does not flash. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <gui/pixel.h>
#include "comp.h"

#define FLASH_MS 300
#define MAX_FLASHES 64
#define OUTLINE_COLOR 0x00ff00ffu
#define FLASH_COLOR 0x00ff0000u
#define OPAQUE_COLOR 0x0000c000u

static int highlight_id;
static struct wire_resource *highlight_owner;
static struct {
    struct rect r;
    long until;
} flashes[MAX_FLASHES];
static int nflashes;
static struct rect_set new_damage;      /* damage of this frame from other sources than the views */
static int internal;                    /* the views add damage */
static int frames, fps_shown = -1;
static long fps_next;

/* The rectangle of the frame counter, logical pixels. */
static struct rect fps_rect(void)
{
    return (struct rect){ screen_w - 88, 4, 84, 20 };
}

static void damage_internal(struct rect r)
{
    internal = 1;
    scene_damage(r);
    internal = 0;
}

void overlay_note_damage(struct rect r)
{
    if (!internal)
        rect_set_add(&new_damage, r);
}

static struct csurface *find_surface(int id)
{
    for (struct csurface *s = surface_first(); s; s = s->next)
        if (s->id == id)
            return s;
    return NULL;
}

/* The outlined part of a surface: the frame of a decorated window, else
 * the surface. The outline lies inside it, so the damage of the surface
 * covers the outline. */
static struct rect outline_rect(const struct csurface *s)
{
    return decor_has(s) ? decor_opaque(s) : surface_rect(s);
}

void overlay_highlight(struct wire_resource *owner, int id)
{
    struct csurface *old = find_surface(highlight_id), *now = find_surface(id);
    if (old)
        damage_internal(outline_rect(old));
    highlight_id = now ? id : 0;
    highlight_owner = now ? owner : NULL;
    if (now)
        damage_internal(outline_rect(now));
}

void overlay_owner_gone(struct wire_resource *owner)
{
    if (owner && owner == highlight_owner)
        overlay_highlight(NULL, 0);
}

void overlay_surface_gone(struct csurface *s)
{
    if (s->id == highlight_id) {
        highlight_id = 0;
        highlight_owner = NULL;
    }
}

void overlay_settings_changed(void)
{
    if (!settings.debug_damage)
        nflashes = 0;
    if (settings.debug_fps) {
        fps_next = uptime_ms() + 1000;
        frames = 0;
        fps_shown = -1;
    }
    internal = 1;
    scene_damage_all();
    internal = 0;
}

/* Called before a frame composes, so that the frame draws its own
 * flashes. */
void overlay_frame_begin(void)
{
    if (new_damage.n)
        frames++;
    if (settings.debug_damage) {
        long until = uptime_ms() + FLASH_MS;
        for (int i = 0; i < new_damage.n && nflashes < MAX_FLASHES; i++) {
            flashes[nflashes].r = new_damage.r[i];
            flashes[nflashes++].until = until;
        }
    }
    rect_set_clear(&new_damage);
}

long overlay_next_deadline(void)
{
    long next = -1;
    for (int i = 0; i < nflashes; i++)
        if (next < 0 || flashes[i].until < next)
            next = flashes[i].until;
    if (settings.debug_fps && (next < 0 || fps_next < next))
        next = fps_next;
    return next;
}

void overlay_tick(long now)
{
    for (int i = 0; i < nflashes;) {
        if (flashes[i].until <= now) {
            damage_internal(flashes[i].r);
            flashes[i] = flashes[--nflashes];
        } else {
            i++;
        }
    }
    if (settings.debug_fps && now >= fps_next) {
        if (frames != fps_shown)
            damage_internal(fps_rect());
        fps_shown = frames;
        frames = 0;
        fps_next = now + 1000;
    }
}

/* Blend colour with alpha over the device pixels of the logical
 * rectangle r inside clip. */
static void tint(struct rect r, struct rect clip, uint32_t color, uint32_t alpha)
{
    struct rect R = rect_scale(rect_intersect(r, clip), screen_scale);
    for (int y = R.y; y < R.y + R.h; y++) {
        uint32_t *row = back.pixels + (size_t)y * back.stride;
        for (int x = R.x; x < R.x + R.w; x++)
            row[x] = pixel_blend(row[x], color, alpha);
    }
}

static void fill(struct rect r, struct rect clip, uint32_t color)
{
    struct rect R = rect_scale(rect_intersect(r, clip), screen_scale);
    gfx_fill_rect(&back, R.x, R.y, R.w, R.h, color);
}

void overlay_draw(struct rect clip, struct csurface **order, int n)
{
    if (settings.debug_opaque)
        for (int i = 0; i < n; i++) {
            struct csurface *s = order[i];
            if (!s->current.buffer)
                continue;
            if (s->current.buffer->format == FORMAT_XRGB8888) {
                tint(surface_rect(s), clip, OPAQUE_COLOR, 70);
                continue;
            }
            for (int q = 0; q < s->current.nopaque; q++) {
                struct rect o = s->current.opaque[q];
                tint((struct rect){ o.x + s->x, o.y + s->y, o.w, o.h }, clip, OPAQUE_COLOR, 70);
            }
        }
    for (int i = 0; i < nflashes; i++)
        tint(flashes[i].r, clip, FLASH_COLOR, 90);
    struct csurface *h = highlight_id ? find_surface(highlight_id) : NULL;
    if (h && h->mapped) {
        struct rect o = outline_rect(h);
        fill((struct rect){ o.x, o.y, o.w, 2 }, clip, OUTLINE_COLOR);
        fill((struct rect){ o.x, o.y + o.h - 2, o.w, 2 }, clip, OUTLINE_COLOR);
        fill((struct rect){ o.x, o.y, 2, o.h }, clip, OUTLINE_COLOR);
        fill((struct rect){ o.x + o.w - 2, o.y, 2, o.h }, clip, OUTLINE_COLOR);
    }
    if (settings.debug_fps) {
        struct rect f = fps_rect();
        if (rect_empty(rect_intersect(f, clip)))
            return;
        fill(f, clip, 0x00202020);
        char text[24];
        snprintf(text, sizeof text, "%d fps", fps_shown < 0 ? 0 : fps_shown);
        /* The text is drawn whole, clipped to the rectangle. */
        struct rect F = rect_scale(rect_intersect(f, clip), screen_scale);
        struct surface v = { back.pixels + (size_t)F.y * back.stride + F.x, F.w, F.h, back.stride };
        int S = screen_scale;
        gfx_text_font_scaled(&v, gfx_font_builtin(), (f.x + 6) * S - F.x, (f.y + 2) * S - F.y, text, 0x00ffffff,
                             0xffffffffu, S);
    }
}
