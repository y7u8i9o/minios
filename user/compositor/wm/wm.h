#pragma once
/* The window manager module of X12 (docs/design/compositor.md).
 *
 * The module decides the placement, the stacking order, the activation
 * and the states of toplevels. The module draws the server side
 * decorations, the shadows and the dimming of the windows of unresponsive
 * clients. The module handles the move and resize drags and the window
 * shortcuts Alt+Tab, Alt+F4 and Alt+drag.
 *
 * The core of X12 calls the functions of this header. The module calls
 * the core through comp.h. The module runs in the thread of the core and
 * shares its state without a lock. */
#include <stdint.h>
#include <gui/gfx.h>

struct csurface;
struct toplevel;

/* The answers of wm_pointer_press. */
enum { WM_PASS, WM_USED, WM_BLOCKED };

/* Load the title font. */
void wm_init(void);

/* ---- events of the core ---- */

/* A surface received the toplevel role. */
void wm_toplevel_created(struct toplevel *t);
/* A toplevel committed a buffer. first_map is 1 for the first buffer. */
void wm_toplevel_committed(struct toplevel *t, int first_map);
/* The role object of t ends. The surface of t has no role anymore, and
 * the core frees t after the call. */
void wm_toplevel_destroyed(struct toplevel *t, int was_active);
/* The screen size or the desktop area changed. */
void wm_output_changed(void);
/* The client of t asked for a move or a resize drag with a valid serial. */
void wm_request_move(struct toplevel *t);
void wm_request_resize(struct toplevel *t, int edges);

/* ---- input ---- */

/* A button press over target (NULL over the desktop). WM_USED: the press
 * is consumed. WM_BLOCKED: a modal window blocks target, and no surface
 * receives the press. WM_PASS: the core delivers the press to target. */
int wm_pointer_press(struct csurface *target, int button);
/* 1 while a move or resize drag of the module takes the pointer. */
int wm_pointer_motion(void);
/* The end of a drag: 1 for a drag of the module, 2 for a drag that the
 * client requested (the client then receives the release), 0 otherwise. */
int wm_pointer_release(void);
/* 1 when a window shortcut consumed the key press. */
int wm_key(uint32_t key, int mods);
/* The cursor name of the window manager at the pointer over target (the
 * surface under the pointer, or NULL), or NULL when the client decides the
 * cursor (docs/design/cursors.md). */
const char *wm_cursor(struct csurface *target);

/* ---- drawing ---- */

/* The area that the scene draws for s: the surface, or the frame with the
 * shadow of a decorated toplevel. */
struct rect wm_extent(const struct csurface *s);
/* The part of wm_extent that is opaque for an opaque buffer. */
struct rect wm_opaque(const struct csurface *s);
/* Draw the shadow and the decorations of s inside clip, below the surface. */
void wm_draw_below(struct csurface *s, struct rect clip);
/* Draw the dimming of an unresponsive client over s inside clip. */
void wm_draw_above(struct csurface *s, struct rect clip);

/* ---- window actions ---- */

/* The visible frame of a toplevel: the server decorations, the window
 * geometry of the client, or the surface. */
struct rect toplevel_frame(const struct toplevel *t);
/* The size that a configure describes: the surface for server
 * decorations, the window geometry for client decorations. */
void toplevel_configure_size(const struct toplevel *t, int *w, int *h);
/* The activated toplevel, or NULL. */
struct toplevel *toplevel_focused(void);
void toplevel_activate(struct toplevel *t);
void toplevel_set_maximized(struct toplevel *t, int on);
void toplevel_set_minimized(struct toplevel *t, int on);
/* Ask the client to close t, or the modal child of t. */
void toplevel_close(struct toplevel *t);
