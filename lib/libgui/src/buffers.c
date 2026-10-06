/* Window buffer slots: buffer-age copies and the raw corner store
 * (buffers.h). */
#include "buffers.h"
#include <stdlib.h>
#include <string.h>

struct surface gui_buffers_surface(const struct gui_buffers *b)
{
    const struct gui_slot *s = &b->slot[b->cur];
    return (struct surface){ s->pixels, s->w, s->h, s->w };
}

int gui_buffers_set_corners(struct gui_buffers *b, const struct rect *corners, int n)
{
    size_t total = 0;
    for (int i = 0; i < n; i++)
        total += (size_t)corners[i].w * corners[i].h;
    uint32_t *store = total ? calloc(total, 4) : NULL;
    if (total && !store)
        return -1;
    free(b->corner_raw);
    b->corner_raw = store;
    b->ncorners = n;
    memcpy(b->corner, corners, (size_t)n * sizeof corners[0]);
    return 0;
}

void gui_buffers_stale_all(struct gui_buffers *b)
{
    for (int i = 0; i < GUI_SLOTS; i++) {
        rect_set_clear(&b->slot[i].stale);
        if (i != b->cur)
            rect_set_add(&b->slot[i].stale, (struct rect){ 0, 0, b->w, b->h });
    }
}

int gui_buffers_pick(const struct gui_buffers *b, int n)
{
    for (int i = 0; i < n && i < b->nslots; i++)
        if (i != b->cur && !b->slot[i].busy && b->slot[i].pixels)
            return i;
    return -1;
}

/* Copy the corner squares between a slot and the store. */
static void corners_copy(struct gui_buffers *b, struct gui_slot *s, int to_store)
{
    uint32_t *store = b->corner_raw;
    for (int k = 0; k < b->ncorners; k++) {
        struct rect r = b->corner[k];
        for (int y = 0; y < r.h; y++) {
            uint32_t *px = s->pixels + (size_t)(r.y + y) * s->w + r.x;
            if (r.y + y >= 0 && r.y + y < s->h && r.x >= 0 && r.x + r.w <= s->w) {
                if (to_store)
                    memcpy(store, px, (size_t)r.w * 4);
                else
                    memcpy(px, store, (size_t)r.w * 4);
            }
            store += r.w;
        }
    }
}

size_t gui_buffers_switch(struct gui_buffers *b, int t)
{
    struct gui_slot *from = &b->slot[b->cur], *to = &b->slot[t];
    struct rect all = { 0, 0, b->w, b->h };
    size_t bytes = 0;
    for (int i = 0; i < to->stale.n; i++) {
        struct rect r = rect_intersect(to->stale.r[i], all);
        bytes += (size_t)r.w * r.h * 4;
        for (int y = r.y; y < r.y + r.h; y++)
            memcpy(to->pixels + (size_t)y * to->w + r.x, from->pixels + (size_t)y * from->w + r.x, (size_t)r.w * 4);
    }
    rect_set_clear(&to->stale);
    b->cur = t;
    b->committed = 0;
    if (b->corner_raw)
        corners_copy(b, to, 0);
    return bytes;
}

void gui_buffers_commit(struct gui_buffers *b, const struct rect_set *d)
{
    for (int i = 0; i < GUI_SLOTS; i++)
        if (i != b->cur)
            for (int k = 0; k < d->n; k++)
                rect_set_add(&b->slot[i].stale, d->r[k]);
    if (b->corner_raw)
        corners_copy(b, &b->slot[b->cur], 1);
    b->slot[b->cur].busy = 1;
    b->committed = 1;
}

void gui_buffers_put_raw_corners(const struct gui_buffers *b, struct surface *dst, int dx, int dy)
{
    const uint32_t *store = b->corner_raw;
    if (!store)
        return;
    struct rect whole = { 0, 0, dst->width, dst->height };
    for (int k = 0; k < b->ncorners; k++) {
        struct rect c = b->corner[k];
        struct rect r = rect_intersect((struct rect){ c.x + dx, c.y + dy, c.w, c.h }, whole);
        for (int y = r.y; y < r.y + r.h; y++)
            memcpy(dst->pixels + (size_t)y * dst->stride + r.x,
                   store + (size_t)(y - dy - c.y) * c.w + (r.x - dx - c.x), (size_t)r.w * 4);
        store += (size_t)c.w * c.h;
    }
}

void gui_buffers_free(struct gui_buffers *b)
{
    free(b->corner_raw);
    b->corner_raw = NULL;
    b->ncorners = 0;
}
