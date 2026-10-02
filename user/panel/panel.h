#pragma once
/* Panel internals shared by panel.c (the bar, the launcher menu, the
 * task buttons) and mixer.c (the audio applet). */
#include <stdint.h>
#include <wire/client.h>
#include <gui/gfx.h>
#include <gui/paint.h>
#include <gui/theme.h>
#include "core-client.h"
#include "shell-client.h"
#include "seat-client.h"

#define PANEL_H 28
#define MENU_BTN_W 64
#define TASK_BTN_W 120
#define CLOCK_W 80
#define MIXER_BTN_W 30
#define INPUT_W 30                      /* the label of the layout or input method */
#define MENU_ITEM_H 24
#define MENU_PAD 6
#define LAUNCHER_COLUMN_W 200
#define LAUNCHER_SEARCH_H 34
#define LAUNCHER_HEADING_H 22
#define LAUNCHER_RULE_H 9
#define LAUNCHER_ICON 16

/* Colours: a dark neutral bar, pill shaped buttons, a light menu. */
#define PANEL_BG        0x0023272c
#define PANEL_LINE      0x00343a41
#define PANEL_TEXT      0x00e6e8eb
#define PANEL_TEXT_DIM  0x00a0a6ae
#define BUTTON_BG       0x002e343b
#define BUTTON_ACTIVE   0x003f4854
#define BUTTON_OPEN     0x004a5563
#define ACCENT          0x005b9cf5
#define MENU_BG         0x00fafbfc
#define MENU_BORDER     0x00c5cad1
#define MENU_HOVER      0x00dce8fa
#define MENU_TEXT       0x00202428
#define MENU_TEXT_DIM   0x00858b93
#define MENU_FIELD      0x00ffffff
#define MENU_ICON       0x00454b53
#define METER_BG        0x00e3e6ea
#define METER_FG        0x0047b26b

/* Positioner anchors and gravities. */
#define POS_TOP_LEFT 5
#define POS_TOP_RIGHT 7

/* A drawable surface backed by one shm buffer of lw by lh logical
 * pixels at the output scale. */
struct canvas {
    struct wire_proxy *surface, *buffer, *pool;
    struct surface s;
    int fd;
    int lw, lh, scale;
};

extern struct wire_display *display;
extern struct wire_proxy *compositor, *shm, *shell, *seat;
extern struct canvas panel;
extern struct theme ui;
extern int screen_w, screen_h, output_scale;
extern uint32_t press_serial;

int canvas_create(struct canvas *c, int w, int h);
void canvas_release_buffer(struct canvas *c);
void canvas_commit(struct canvas *c);
void canvas_painter(struct painter *p, struct canvas *c);
/* A label vertically centred in a box, clipped to it, optionally
 * centred horizontally. */
void panel_label(struct painter *p, int x, int y, int w, int h, const char *text, uint32_t color, int centre);
void log_line(const char *fmt, ...);
void draw_panel(void);

/* The launcher menu (launcher.c): a popup above the Menu button with a
 * search field and the entries of /etc/launcher and of the installed
 * packages. */
void launcher_init(void);
int launcher_is_open(void);
void launcher_toggle(void);
int launcher_is_surface(const struct wire_proxy *surface);
void launcher_pointer_motion(int x, int y);
void launcher_pointer_button(uint32_t button, uint32_t state, int x, int y);

/* The audio applet: a button left of the clock and a popup with the
 * master volume and one row per stream of the audio server. */
int mixer_button_x(void);
void mixer_draw_button(struct painter *p);
int mixer_is_open(void);
void mixer_toggle(void);
int mixer_owns(const struct wire_proxy *surface);
void mixer_pointer_motion(int x, int y);
void mixer_pointer_button(uint32_t button, uint32_t state, int x, int y);
/* The audio connection to poll while the popup is open, or -1. */
int mixer_fd(void);
void mixer_dispatch(int revents);
