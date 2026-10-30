#pragma once
/* The declarations that the files of the window manager module share.
 * The core of X12 does not include this header. */
#include "../comp.h"

#define TITLE_H 28
#define BORDER 1
#define GRIP 12
#define TITLE_BTN 14
#define RADIUS 8                /* rounded top corners of the frame */
#define SHADOW 14               /* reach of the drop shadow around the frame */
#define SHADOW_DY 3             /* the shadow is shifted down by this much */
#define RESIZE_MARGIN 6         /* invisible resize zone outside the frame */

/* window.c */
void toplevel_move(struct toplevel *t, int x, int y);

/* decor.c */
void decor_init(void);                                     /* loads the title font */
int decor_has(const struct csurface *s);
struct rect decor_frame(const struct csurface *s);         /* frame including the title bar */
struct rect decor_extent(const struct csurface *s);        /* frame plus server-side shadow */
struct rect decor_opaque(const struct csurface *s);        /* part of the frame without rounded corners */
void decor_draw_shadow(struct csurface *s, struct rect clip);
void decor_draw(struct csurface *s, struct rect clip);
/* Free what decor.c stores for a toplevel that goes away. */
void decor_free(struct toplevel *t);
/* Whether the point lies on the decorations of s: the frame, the resize
 * margins around the frame and the grip. */
int decor_hit(const struct csurface *s, int x, int y);
/* Returns 1 when the press at the cursor was consumed by decorations.
 * button: 1 is the left button over the decorations. Bit 0x100 marks a
 * move requested by the client, bit 0x200 a resize with the edges in
 * bits 16 and up. */
int decor_press(struct csurface *s, int button);
int decor_motion(void);
int decor_release(void);
/* A toplevel ends. A drag of the toplevel ends with it. */
void decor_toplevel_gone(struct toplevel *t);
