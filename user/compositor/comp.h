#pragma once
/* The compositor: surfaces with shared memory buffers (M24), roles,
 * decorations, seat and data device (M25). Single threaded. */
#include <stdint.h>
#include <wire/server.h>
#include <gui/gfx.h>
#include "core-server.h"
#include "shell-server.h"
#include "seat-server.h"
#include "data-server.h"
#include "text-server.h"
#include "debug-server.h"

#define FORMAT_XRGB8888 1
#define FORMAT_ARGB8888 2
#define MAX_DAMAGE 16
#define MAX_REGION 32
#define FRAME_MS 16
#define TITLE_H 20
#define BORDER 1
#define GRIP 12
#define TITLE_BTN 14
#define SHADOW 4

enum role { ROLE_NONE, ROLE_TOPLEVEL, ROLE_POPUP, ROLE_LAYER, ROLE_CURSOR, ROLE_DND_ICON };
enum { DECOR_SERVER = 1, DECOR_CLIENT = 2 };
enum { STATE_MAXIMIZED = 1, STATE_ACTIVATED = 2, STATE_MINIMIZED = 3 };
enum { ANCHOR_NONE, ANCHOR_TOP, ANCHOR_BOTTOM, ANCHOR_LEFT, ANCHOR_RIGHT, ANCHOR_TOP_LEFT, ANCHOR_BOTTOM_LEFT,
       ANCHOR_TOP_RIGHT, ANCHOR_BOTTOM_RIGHT };
enum { LAYER_ANCHOR_TOP = 1, LAYER_ANCHOR_BOTTOM = 2, LAYER_ANCHOR_LEFT = 4, LAYER_ANCHOR_RIGHT = 8 };
enum { EDGE_TOP = 1, EDGE_BOTTOM = 2, EDGE_LEFT = 4, EDGE_RIGHT = 8 };
enum { ADJUST_FLIP_X = 1, ADJUST_FLIP_Y = 2, ADJUST_SLIDE_X = 4, ADJUST_SLIDE_Y = 8,
       ADJUST_RESIZE_X = 16, ADJUST_RESIZE_Y = 32 };

struct pool {
    struct wire_resource *res;
    int fd;
    uint8_t *map;
    size_t size;
    int refs;
};

struct buffer {
    struct wire_resource *res;
    struct pool *pool;
    int offset, width, height, stride;
    uint32_t format;
    int busy;
};

struct surface_state {
    struct buffer *buffer;
    int has_buffer;
    struct rect damage[MAX_DAMAGE];
    int ndamage;
    struct wire_resource *callbacks[8];
    int ncallbacks;
    struct rect opaque[MAX_REGION], input[MAX_REGION];
    int nopaque, ninput;
    int opaque_set, input_set;
    int attach_x, attach_y;
    int scale, transform;
    int state_set;
};

struct positioner {
    struct wire_resource *res;
    int w, h, ax, ay, aw, ah, anchor, gravity, ox, oy, adjust;
};

struct toplevel {
    struct csurface *s;
    struct wire_resource *res;
    char title[64], app_id[64];
    int min_w, min_h, max_w, max_h;
    int maximized, minimized, activated;
    int saved_x, saved_y, saved_w, saved_h;
    uint32_t configure_serial, acked_serial;
    int pending_w, pending_h;               /* size of the last configure */
    int decor_mode;
    struct wire_resource *decoration;
    int handle_count;
    int number;                             /* creation order, used by the logs */
    int placed;
    struct toplevel *parent;
    int modal;
};

struct popup {
    struct csurface *s, *parent;
    struct wire_resource *res;
    int x, y;                               /* relative to the parent */
    int grab;
    struct positioner pos;
    uint32_t serial;
    uint32_t acked_serial;
    int pending_w, pending_h;
};

struct layer {
    struct csurface *s;
    struct wire_resource *res;
    int anchor, exclusive, w, h, interactive;
    uint32_t layer;
    uint32_t serial;
    uint32_t acked_serial;
    int pending_w, pending_h;
};

struct csurface {
    struct wire_resource *res;
    struct client *client;
    int id;
    int x, y, width, height;
    struct surface_state pending, current;
    struct wire_resource *frame_cbs[8];
    int nframe_cbs;
    int mapped;
    enum role role;
    struct toplevel *toplevel;
    struct popup *popup;
    struct layer *layer;
    int stack;                              /* z order among toplevels, higher on top */
    int hotspot_x, hotspot_y;               /* cursor surfaces */
    struct csurface *next;
};

struct client {
    struct wire_client *wc;
    int number;
    long stall_since;                       /* uptime when its socket first stayed full */
    struct wire_resource *pointer, *keyboard, *data_device;
    struct wire_resource *text_input;
    uint32_t input_serials[16];
    int ninput_serials;
    struct wire_resource *seat_res;
    struct wire_resource *manager;
};

/* debug.c: tunable settings */
struct comp_settings {
    int frame_ms, desktop_color, repeat_rate, repeat_delay, decor_default, verbose;
    int display_mode;               /* DISPLAY_MODE_PACK of the current mode */
};
/* display_mode packs width, height (up to 16383) and the pixel scale (1..4). */
#define DISPLAY_MODE_PACK(w, h, s) (((s) << 28) | ((w) << 14) | (h))
#define DISPLAY_MODE_W(m) (((m) >> 14) & 0x3fff)
#define DISPLAY_MODE_H(m) ((m) & 0x3fff)
#define DISPLAY_MODE_S(m) (((m) >> 28) & 7)
extern struct comp_settings settings;
void debug_init(struct wire_server *srv);
void frame_clock_set(int ms);                   /* main.c */
void seat_repeat_changed(void);                 /* seat.c */
void scene_stat_values(long *count, long *ms, long *max);   /* scene.c */

/* main.c */
extern int screen_w, screen_h;
extern int screen_scale;                /* framebuffer pixels per logical pixel */
extern int cursor_x, cursor_y;
void comp_log(const char *fmt, ...);
uint32_t comp_serial(void);
/* surface.c */
void surfaces_init(struct wire_server *srv);
struct csurface *surface_first(void);
struct csurface *surface_by_resource(struct wire_resource *r);
void surface_unmap(struct csurface *s);
struct rect surface_rect(const struct csurface *s);
int surface_accepts_input(const struct csurface *s, int x, int y);
int surface_commit_allowed(struct wire_client *c, struct csurface *s, struct buffer *b);
/* scene.c */
void scene_init(void);
void scene_damage(struct rect r);
void scene_damage_all(void);
int scene_has_damage(void);
void scene_compose(void);
void scene_set_cursor(int x, int y);
void scene_cursor_changed(void);
void scene_stats(void);
struct csurface *scene_surface_at(int x, int y);           /* content hit, topmost */
int scene_order(struct csurface **out, int max);           /* bottom to top */
/* backend_fb.c */
extern struct surface back;
int backend_init(void);
void backend_flush(struct rect r);
int backend_can_set_mode(void);
/* Change the framebuffer mode; screen_w, screen_h, screen_scale and the
 * back buffer follow. */
int backend_set_mode(int width, int height, int scale);
/* main.c: apply a mode to the backend, the shell and every client. */
int comp_set_mode(int width, int height, int scale);
/* surface.c: re-announce the output to every bound output resource. */
void output_changed(void);
/* shell.c: the screen size changed; re-layout layers, clamp windows. */
void shell_output_changed(void);
void backend_release(void);
/* shell.c */
void shell_init(struct wire_server *srv);
void shell_surface_committed(struct csurface *s, int first_map);
void shell_surface_gone(struct csurface *s);
struct rect shell_desktop(void);                           /* area left by layer surfaces */
void toplevel_activate(struct toplevel *t);
void toplevel_set_maximized(struct toplevel *t, int on);
void toplevel_set_minimized(struct toplevel *t, int on);
void toplevel_close(struct toplevel *t);
void toplevel_configure(struct toplevel *t, int w, int h);
void toplevel_move(struct toplevel *t, int x, int y);
struct toplevel *toplevel_focused(void);
int toplevel_blocked(struct toplevel *t);        /* activates its modal child */
void toplevel_cycle(void);
void popup_dismiss_all(void);
struct csurface *popup_grab_surface(void);
/* decor.c */
struct rect decor_frame(const struct csurface *s);         /* frame including the title bar */
struct rect decor_extent(const struct csurface *s);        /* frame plus server-side shadow */
int decor_has(const struct csurface *s);
void decor_draw_shadow(struct csurface *s, struct rect clip);
void decor_draw(struct csurface *s, struct rect clip);
/* Returns 1 when the press at the cursor was consumed by decorations. */
int decor_press(struct csurface *s, int button);
int decor_motion(void);
int decor_release(void);
int decor_dragging(void);
/* seat.c */
void seat_init(struct wire_server *srv);
void seat_pointer_motion(void);
void seat_pointer_button(int button, int pressed);
void seat_pointer_axis(int delta);
void seat_key(uint32_t key, int pressed);
void seat_set_keyboard_focus(struct csurface *s);
struct csurface *seat_keyboard_focus(void);
void seat_surface_gone(struct csurface *s);
uint32_t seat_last_serial(void);
int seat_modifiers(void);
int seat_validate_serial(struct client *client, uint32_t serial);
int seat_validate_grab(struct client *client, struct csurface *origin, uint32_t serial);
int seat_validate_drag(struct client *client, struct csurface *origin, uint32_t serial);
struct csurface *seat_cursor_surface(void);
int seat_cursor_hidden(void);
int seat_translate(uint32_t key, int mods);
/* text.c */
void text_init(struct wire_server *srv);
void text_focus_changed(struct csurface *old, struct csurface *now);
void text_key(uint32_t key, int pressed, int mods);
void text_surface_gone(struct csurface *s);
void text_client_gone(struct client *c);
/* data.c */
void data_init(struct wire_server *srv);
void data_keyboard_focus_changed(struct client *c);
void data_pointer_motion(void);
void data_pointer_release(void);
int data_dragging(void);
void data_surface_gone(struct csurface *s);
void data_client_gone(struct client *c);
int data_fetch_fd(void);                 /* -1 when no transfer is in progress */
void data_fetch_read(void);
