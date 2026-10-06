#pragma once
/* Window buffer slots without a private drawing surface (G8 of
 * docs/plan/compositor-performance.md). A window draws straight into one
 * slot of its shared memory pool. After a commit the compositor may read
 * that slot, so the next paint first moves to a free slot: it copies the
 * regions that changed since that slot was last current, and it restores
 * the raw pixels of the rounded frame corners, which the commit blended
 * over the chrome. This file contains no protocol calls, so the host
 * tests can run it. */
#include <gui/gfx.h>

#define GUI_SLOTS 3

struct gui_slot {
    uint32_t *pixels;           /* slot memory, stride w */
    int w, h;                   /* geometry of the slot's contents, 0 before first use */
    int busy;                   /* the compositor may read the slot */
    struct rect_set stale;      /* regions that differ from the current slot */
};

struct gui_buffers {
    struct gui_slot slot[GUI_SLOTS];
    int nslots;                 /* slots with memory */
    int cur;                    /* slot with the newest contents */
    int committed;              /* cur was committed since it became current */
    int w, h;                   /* window buffer size in device pixels */
    /* Raw pixels of the rounded frame corners, which a commit replaces in
     * the slot with their blend over the chrome. */
    struct rect corner[4];
    int ncorners;
    uint32_t *corner_raw;
};

/* The current slot as a surface. */
struct surface gui_buffers_surface(const struct gui_buffers *b);
/* Set the corner squares (device pixels) and allocate their store.
 * Returns -1 without memory. */
int gui_buffers_set_corners(struct gui_buffers *b, const struct rect *corners, int n);
/* Every slot but the current one differs everywhere from the current one. */
void gui_buffers_stale_all(struct gui_buffers *b);
/* A slot other than the current one that the compositor does not read,
 * among the first n slots; -1 if none. */
int gui_buffers_pick(const struct gui_buffers *b, int n);
/* Make slot t current: copy its stale regions from the current slot and
 * restore the raw corners. Slot t must have the window geometry. Returns
 * the number of bytes copied. */
size_t gui_buffers_switch(struct gui_buffers *b, int t);
/* Record a commit of the current slot with the damage d: the other slots
 * become stale there, the raw corners are saved, and the slot counts as
 * read by the compositor. The caller then blends the corners. */
void gui_buffers_commit(struct gui_buffers *b, const struct rect_set *d);
/* Write the raw corner pixels of the last commit into dst. Each corner
 * square moves by (dx, dy) and is clipped to dst. A resize uses this
 * function after it copies committed contents into a new slot. */
void gui_buffers_put_raw_corners(const struct gui_buffers *b, struct surface *dst, int dx, int dy);
void gui_buffers_free(struct gui_buffers *b);
