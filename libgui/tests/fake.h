#pragma once
#include <gui/gfx.h>
#include <gui/client.h>
extern struct rect fake_last_damage;
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
