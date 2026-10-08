#pragma once
#include <gui/gfx.h>
#include <gui/client.h>
extern struct rect fake_last_damage;
/* The scale of the output and of the windows created from now on. A test
 * that changes the scale sets it back to 1 at its end. */
extern int fake_scale;
/* The device pixels of all damage since the start. */
extern uint64_t fake_damage_pixels;
extern int fake_damage_count;
void fake_push(const struct wmsg *m);
struct fake_drag {
    int started, window, nitems, actions, icon;
    char mime[8][64];
    char data[8][512];
};
extern struct fake_drag fake_drag;
extern const char *fake_offers[8];
extern const char *fake_peek;
extern const char *fake_accept_mime;
extern int fake_accept_actions, fake_accept_preferred;
extern const char *fake_drop, *fake_drop_mime;
/* The opaque region set last, and its rectangle count (-1 for none). */
extern struct rect fake_opaque[8];
extern int fake_nopaque;
