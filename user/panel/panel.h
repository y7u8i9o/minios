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

/* The geometry of the bar. kernel/tests/gui_helpers.h repeats it for the
 * boot tests. */
#define PANEL_H 28
#define BUTTON_Y 4                      /* buttons are PANEL_H - 2 * BUTTON_Y high */
#define MENU_BTN_W 76                   /* the icon and "Menu" */
#define TASKS_X (4 + MENU_BTN_W + 8)    /* the first window button */
#define TASK_BTN_W 160                  /* a window button when there is room */
#define TASK_MIN_W 40                   /* the narrowest window button */
#define TASK_ICON_ONLY_W 72             /* below this width a button shows its icon alone */
#define TASK_GAP 4
#define CLOCK_W 128                     /* the date and the time */
#define MIXER_BTN_W 30
#define POWER_BTN_W 30                  /* the power button left of the show desktop button */
#define DESKTOP_BTN_W 24                /* the show desktop button at the right edge */
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
#define BUTTON_HOVER    0x00384049
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
#define POS_BOTTOM_LEFT 6
#define POS_TOP_RIGHT 7
#define POS_BOTTOM_RIGHT 8

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
/* The panel is at the top of the screen (panel_position=top in
 * desktop.conf), else at the bottom. */
extern int panel_at_top;
/* Places a popup at the button from x to x + w of the bar: above it on a
 * bottom panel, below it on a top panel. right aligns the right edges of
 * the popup and the button, else their left edges. */
void panel_place_popup(struct wire_proxy *pos, int x, int w, int right);
/* The icons of the panel (icons.c): the SVG icon NAME of /usr/share/icons
 * in color at the output scale, and the icon of the program of a command
 * or an app_id (launcher_icon_name). NULL when the file is missing. */
const struct image *panel_icon(const char *name, uint32_t color);
const struct image *panel_app_icon(const char *command, uint32_t color);

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
void mixer_draw_button(struct painter *p, int hovered);
int mixer_is_open(void);
void mixer_toggle(void);
int mixer_owns(const struct wire_proxy *surface);
void mixer_pointer_motion(int x, int y);
void mixer_pointer_button(uint32_t button, uint32_t state, int x, int y);
/* The power button at the right end of the bar and its menu (power.c):
 * Log out, Restart and Shut down. */
int power_x(void);
int power_is_open(void);
int power_owns(const struct wire_proxy *surface);
void power_draw_button(struct painter *p, int hovered);
void power_toggle(void);
void power_pointer_motion(int x, int y);
void power_pointer_button(uint32_t button, uint32_t state, int x, int y);
/* The clock left of the power button and its calendar (calendar.c): a
 * popup above the clock with a month, opened and closed by a click on the
 * clock. */
int clock_x(void);
int calendar_is_open(void);
int calendar_owns(const struct wire_proxy *surface);
void calendar_toggle(void);
void calendar_pointer_button(uint32_t button, uint32_t state, int x, int y);
/* The input method menu (imemenu.c): the label of the layout or input
 * method left of the mixer button and a popup that selects one. */
int imemenu_x(void);
void imemenu_bind(struct wire_proxy *manager);
void imemenu_toggle(void);
int imemenu_owns(const struct wire_proxy *surface);
void imemenu_pointer_button(uint32_t button, uint32_t state, int x, int y);
/* The audio connection to poll while the popup is open, or -1. */
int mixer_fd(void);
void mixer_dispatch(int revents);
