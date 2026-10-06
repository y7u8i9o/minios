/* Host test of buffers.c (G8 of docs/plan/compositor-performance.md).
 * The simulation follows client.c without the protocol. It paints random
 * rectangles into the current slot and into a reference picture. It
 * repaints the header, commits, resizes and changes the scale. A slot
 * pool that is too small or has no free slot is replaced, as new_pool
 * does. The compositor releases each committed buffer after a random
 * number of later commits.
 *
 * Each commit must present the reference with blended frame corners. A
 * buffer that the compositor still reads must not change, also across
 * two resizes without a commit between them. */
#include "check.h"
#include <stdlib.h>
#include <string.h>
#include <gui/gfx.h>
#include "../src/buffers.h"
#include "../src/csd.h"

static uint32_t rng_state = 99;

static uint32_t rng(void)
{
    rng_state = rng_state * 1103515245u + 12345u;
    return rng_state >> 16 ^ rng_state << 7;
}

/* A committed buffer that the compositor may read. */
struct held {
    const uint32_t *pixels;
    uint32_t *copy;             /* the buffer's pixels at its commit */
    size_t n;
    int gen, slot;              /* pool generation and slot index */
    int left;                   /* later commits before the release */
};

#define MAX_POOLS 64
#define MAX_HELD 8

struct sim {
    struct gui_buffers b;
    struct csd csd;
    int w, h, S;                /* contents in logical pixels, scale */
    size_t content_off;
    uint32_t *pool[MAX_POOLS];  /* old pools remain until the end */
    int gen;                    /* index of the current pool */
    size_t cap;                 /* slot capacity of the current pool in pixels */
    uint32_t *ref;              /* the raw picture of the newest frame */
    struct rect_set damage;
    struct held held[MAX_HELD];
    int nheld;
    uint32_t alpha;             /* alpha of the painted colours */
    int bad_frames, bad_held, pools;
};

static struct surface full_surface(struct sim *s)
{
    return gui_buffers_surface(&s->b);
}

static struct surface ref_surface(struct sim *s)
{
    return (struct surface){ s->ref, s->b.w, s->b.h, s->b.w };
}

static struct surface content_surface(struct sim *s, uint32_t *base)
{
    return (struct surface){ base + s->content_off, s->w * s->S, s->h * s->S, s->b.w };
}

static void release(struct sim *s, int i)
{
    struct held *h = &s->held[i];
    if (h->gen == s->gen)
        s->b.slot[h->slot].busy = 0;
    free(h->copy);
    s->held[i] = s->held[--s->nheld];
}

/* Every buffer that the compositor reads is unchanged. */
static void check_held(struct sim *s)
{
    for (int i = 0; i < s->nheld; i++)
        if (memcmp(s->held[i].pixels, s->held[i].copy, s->held[i].n * 4) != 0)
            s->bad_held++;
}

/* ensure_buffer: the slot gets the window geometry. */
static void ensure_geometry(struct sim *s, int t)
{
    struct gui_slot *slot = &s->b.slot[t];
    if (slot->w == s->b.w && slot->h == s->b.h)
        return;
    slot->w = s->b.w;
    slot->h = s->b.h;
    rect_set_clear(&slot->stale);
    rect_set_add(&slot->stale, (struct rect){ 0, 0, s->b.w, s->b.h });
}

/* surface_resize: contents of w by h at scale S. */
static void sim_resize(struct sim *s, int w, int h, int S)
{
    struct surface old = { 0 }, old_ref = { 0 };
    if (s->ref) {
        old = content_surface(s, s->b.slot[s->b.cur].pixels);
        old_ref = content_surface(s, s->ref);
    }
    int old_committed = s->b.committed;
    int ox = old.stride ? (int)(s->content_off % (size_t)old.stride) : 0;
    int oy = old.stride ? (int)(s->content_off / (size_t)old.stride) : 0;
    uint32_t *old_ref_pixels = s->ref;

    int bw, bh;
    csd_buffer_size(&s->csd, w, h, &bw, &bh);
    s->b.w = bw * S;
    s->b.h = bh * S;
    size_t need = (size_t)s->b.w * s->b.h;
    int t = s->b.nslots && need <= s->cap ? gui_buffers_pick(&s->b, GUI_SLOTS) : -1;
    if (t < 0) {
        s->cap = need + need / 4;
        s->gen = s->pools++;
        s->pool[s->gen] = malloc(s->cap * GUI_SLOTS * 4);
        /* Garbage in fresh memory reveals pixels that nobody paints. */
        for (size_t i = 0; i < s->cap * GUI_SLOTS; i++)
            s->pool[s->gen][i] = rng();
        for (int i = 0; i < GUI_SLOTS; i++)
            s->b.slot[i] = (struct gui_slot){ s->pool[s->gen] + (size_t)i * s->cap, 0, 0, 0, { 0 } };
        s->b.nslots = GUI_SLOTS;
        t = 0;
    }
    ensure_geometry(s, t);
    s->b.cur = t;
    s->b.committed = 0;
    s->w = w;
    s->h = h;
    s->S = S;
    struct rect c = csd_content(&s->csd, w, h);
    s->content_off = (size_t)c.y * S * s->b.w + (size_t)c.x * S;
    s->ref = malloc(need * 4);
    for (size_t i = 0; i < need; i++)
        s->ref[i] = rng();
    struct surface cs = content_surface(s, s->b.slot[t].pixels), cr = content_surface(s, s->ref);
    gfx_fill(&cs, 0x00dcdcdc);
    gfx_fill(&cr, 0x00dcdcdc);
    if (old.pixels) {
        gfx_blit(&cs, 0, 0, &old, NULL);
        if (old_committed)
            gui_buffers_put_raw_corners(&s->b, &cs, -ox, -oy);
        gfx_blit(&cr, 0, 0, &old_ref, NULL);
    }
    free(old_ref_pixels);
    gui_buffers_stale_all(&s->b);
    struct rect corners[4];
    gui_buffers_set_corners(&s->b, corners, csd_corner_rects(&s->csd, w, h, S, corners));
    if (s->csd.enabled) {
        struct surface full = full_surface(s), ref = ref_surface(s);
        csd_paint(&full, S, &s->csd, w, h);
        csd_paint(&ref, S, &s->csd, w, h);
    }
    rect_set_clear(&s->damage);
    rect_set_add(&s->damage, (struct rect){ 0, 0, s->b.w, s->b.h });
}

/* gui_begin_paint: after a commit, move to a free slot. The compositor
 * releases the oldest buffer when no slot is free. */
static void sim_begin(struct sim *s)
{
    while (s->b.committed) {
        int t = gui_buffers_pick(&s->b, 2);
        if (t < 0)
            t = gui_buffers_pick(&s->b, GUI_SLOTS);
        if (t >= 0) {
            ensure_geometry(s, t);
            gui_buffers_switch(&s->b, t);
            return;
        }
        /* The newest buffer remains on screen. */
        int oldest = -1;
        for (int i = 0; i < s->nheld; i++)
            if (!(s->held[i].gen == s->gen && s->held[i].slot == s->b.cur) &&
                (oldest < 0 || s->held[i].left < s->held[oldest].left))
                oldest = i;
        if (oldest < 0)
            return;
        release(s, oldest);
    }
}

/* commit_now and the compositor's releases. */
static void sim_commit(struct sim *s)
{
    check_held(s);
    gui_buffers_commit(&s->b, &s->damage);
    struct surface full = full_surface(s);
    csd_finish_corners(&full, s->S, &s->csd, s->w, s->h);
    rect_set_clear(&s->damage);

    size_t n = (size_t)s->b.w * s->b.h;
    uint32_t *want = malloc(n * 4);
    memcpy(want, s->ref, n * 4);
    struct surface ws = { want, s->b.w, s->b.h, s->b.w };
    csd_finish_corners(&ws, s->S, &s->csd, s->w, s->h);
    if (memcmp(want, full.pixels, n * 4) != 0)
        s->bad_frames++;
    free(want);

    for (int i = 0; i < s->nheld;)
        if (--s->held[i].left <= 0)
            release(s, i);
        else
            i++;
    if (s->nheld == MAX_HELD)
        release(s, 0);
    struct held *h = &s->held[s->nheld++];
    h->pixels = full.pixels;
    h->n = n;
    h->copy = malloc(n * 4);
    memcpy(h->copy, full.pixels, n * 4);
    h->gen = s->gen;
    h->slot = s->b.cur;
    h->left = 1 + (int)(rng() % 3);
}

static void paint_contents(struct sim *s)
{
    struct rect c = rect_scale(csd_content(&s->csd, s->w, s->h), s->S);
    int x = c.x + (int)(rng() % (unsigned)c.w), y = c.y + (int)(rng() % (unsigned)c.h);
    struct rect r = rect_intersect((struct rect){ x, y, 1 + (int)(rng() % 60), 1 + (int)(rng() % 40) }, c);
    /* Half of the rectangles touch a bottom corner of the frame. */
    if (rng() % 2) {
        r.y = c.y + c.h - r.h;
        r.x = rng() % 2 ? c.x : c.x + c.w - r.w;
    }
    uint32_t color = (rng() & 0x00ffffffu) | s->alpha;
    struct surface full = full_surface(s), ref = ref_surface(s);
    gfx_fill_rect(&full, r.x, r.y, r.w, r.h, color);
    gfx_fill_rect(&ref, r.x, r.y, r.w, r.h, color);
    rect_set_add(&s->damage, r);
}

static void run(const char *label, int decorated, uint32_t alpha, int S)
{
    struct sim s;
    memset(&s, 0, sizeof s);
    s.csd.enabled = decorated;
    s.csd.active = 1;
    strcpy(s.csd.title, "test");
    s.alpha = alpha;
    sim_resize(&s, 120, 80, S);
    int resizes = 0;
    for (int step = 0; step < 400 && s.pools < MAX_POOLS; step++) {
        int what = (int)(rng() % 12);
        if (what <= 1) {
            /* Two resizes in a row on every second occasion. */
            for (int k = 0; k < 1 + what; k++) {
                int w = 60 + (int)(rng() % 160), h = 40 + (int)(rng() % 120);
                int scale = rng() % 8 == 0 ? 3 - s.S : s.S;
                sim_resize(&s, w, h, scale);
                check_held(&s);
                resizes++;
            }
        } else {
            sim_begin(&s);
            if (what == 2 && decorated) {
                s.csd.active = !s.csd.active;
                struct surface full = full_surface(&s), ref = ref_surface(&s);
                rect_set_add(&s.damage, csd_paint_header(&full, s.S, &s.csd, s.w, s.h));
                csd_paint_header(&ref, s.S, &s.csd, s.w, s.h);
            } else {
                paint_contents(&s);
            }
        }
        sim_commit(&s);
    }
    CHECK(s.bad_frames == 0, "%s from scale %d: %d presented frames differ from the reference", label, S,
          s.bad_frames);
    CHECK(s.bad_held == 0, "%s from scale %d: %d checks found a changed buffer under the compositor", label, S,
          s.bad_held);
    CHECK(resizes > 20 && s.pools > 1, "%s from scale %d: %d resizes and %d pools exercise the resize path", label,
          S, resizes, s.pools);
    while (s.nheld)
        release(&s, 0);
    gui_buffers_free(&s.b);
    for (int i = 0; i < s.pools; i++)
        free(s.pool[i]);
    free(s.ref);
}

void run_buffers_tests(void)
{
    for (int S = 1; S <= 2; S++) {
        run("A decorated window", 1, 0xff000000u, S);
        run("An opaque window", 0, 0xff000000u, S);
        run("A translucent window", 0, 0x80000000u, S);
        run("A popup", 0, 0xff000000u, S);
    }
}
