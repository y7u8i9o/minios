#pragma once
/* The compositor: surfaces with shared memory buffers (M24), roles,
 * decorations, seat and data device (M25). Single threaded. */
#include <stdint.h>
#include <wire/server.h>
#include <gui/gfx.h>
#include <gui/display.h>
#include "core-server.h"
#include "shell-server.h"
#include "seat-server.h"
#include "data-server.h"
#include "text-server.h"
#include "debug-server.h"
#include "ime-server.h"

#define FORMAT_XRGB8888 1
#define FORMAT_ARGB8888 2
#define MAX_DAMAGE 16
#define MAX_REGION 32
#define FRAME_MS 16
#define TITLE_H 28
#define BORDER 1
#define GRIP 12
#define TITLE_BTN 14
#define RADIUS 8                /* rounded top corners of the frame */
#define SHADOW 14               /* reach of the drop shadow around the frame */
#define SHADOW_DY 3             /* the shadow is shifted down by this much */
#define RESIZE_MARGIN 6         /* invisible resize zone outside the frame */

#define LAYER_OVERLAY 3          /* above the panel, and a keyboard interactive one gets the focus when mapped */
enum role { ROLE_NONE, ROLE_TOPLEVEL, ROLE_POPUP, ROLE_LAYER, ROLE_CURSOR, ROLE_DND_ICON, ROLE_IME_POPUP };
enum { DECOR_SERVER = 1, DECOR_CLIENT = 2 };
enum { STATE_MAXIMIZED = 1, STATE_ACTIVATED = 2, STATE_MINIMIZED = 3 };
enum { ANCHOR_NONE, ANCHOR_TOP, ANCHOR_BOTTOM, ANCHOR_LEFT, ANCHOR_RIGHT, ANCHOR_TOP_LEFT, ANCHOR_BOTTOM_LEFT,
       ANCHOR_TOP_RIGHT, ANCHOR_BOTTOM_RIGHT };
enum { LAYER_ANCHOR_TOP = 1, LAYER_ANCHOR_BOTTOM = 2, LAYER_ANCHOR_LEFT = 4, LAYER_ANCHOR_RIGHT = 8 };
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
    /* The last configures sent, oldest first. A client may acknowledge any
     * of them. The acknowledged one supersedes the older ones. */
    struct { uint32_t serial; int w, h; } sent[4];
    int nsent;
    int acked_w, acked_h;                   /* size of the acknowledged configure */
    int decor_mode;
    struct wire_resource *decoration;
    int handle_count;
    int number;                             /* creation order, used by the logs */
    int placed;
    struct toplevel *parent;
    int modal;
    struct rect geo;                        /* window geometry inside the surface (client decorations) */
    int geo_set;
    /* The title as decor.c last shaped it, at the scale title_shape_scale. */
    struct gfx_shaped *title_shape;
    char title_shape_text[64];
    int title_shape_scale;
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
    int placed_anchor, placed_exclusive;    /* at the last placement */
    int margin[4];                          /* top, right, bottom, left (layer_place) */
    int has_margin;
    uint32_t layer;
    uint32_t serial;
    uint32_t acked_serial;
    int pending_w, pending_h;
    /* The configures sent and not yet acknowledged, oldest first.  A
     * client may acknowledge an older one that it received before a newer
     * one, and may then commit a buffer of its size. */
    struct { uint32_t serial; int w, h; } sent[4];
    int nsent;
    int acked_w, acked_h;
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
    int hotspot_x, hotspot_y;               /* cursor surfaces and drag icons */
    struct csurface *next;
};

struct client {
    struct wire_client *wc;
    int number;
    unsigned uid;                           /* SO_PEERCRED at connect */
    long stall_since;                       /* uptime when its socket first remained full */
    struct wire_resource *pointer, *keyboard, *data_device;
    struct wire_resource *text_input;
    uint32_t input_serials[16];
    int ninput_serials;
    struct wire_resource *seat_res;
    struct wire_resource *manager;
    /* Liveness (hang.c): pings on the shell global, the pid the client
     * reported, and the not responding state. */
    struct wire_resource *shell_res;
    int pid;
    uint32_t ping_serial;
    long ping_sent, last_pong;              /* uptime ms; last_pong is the connect time until the first pong */
    int unresponsive;
    long snooze_until;                      /* Wait pressed: the overlay remains away until then */
};

/* Clients may connect from root, from the user running the server and
 * from the session user, which a root client sets with the setting
 * session_uid (-1 for none) when the greeter starts and ends a session
 * (docs/design/users.md). surface.c. */
extern int session_uid;
int client_uid_allowed(unsigned uid);
/* Disconnect the clients that client_uid_allowed no longer admits. */
void clients_drop_disallowed(void);

/* hang.c: unresponsive clients. */
void hang_init(struct wire_server *srv);
void hang_client_attached(struct client *c);
void hang_tick(long now);
/* The uptime in ms at which hang_tick next has work, or -1 for none (G6). */
long hang_next_deadline(void);
/* Draw the dimming and the dialog over an unresponsive toplevel. */
void hang_draw(struct csurface *s, struct rect clip);
/* A press at (x, y): 1 when it hit an unresponsive window (buttons handled). */
int hang_press(int x, int y);
/* decor.c: the title font and theme for server side text. */
const struct font *decor_font(void);
const struct theme *decor_theme_ptr(void);

/* debug.c: tunable settings */
struct comp_settings {
    int frame_ms, desktop_color, repeat_rate, repeat_delay, decor_default, verbose;
    int display_mode;               /* DISPLAY_MODE_PACK of the current mode */
    int pointer_speed;              /* -100..100, 0 is unscaled */
    int pointer_accel;              /* POINTER_ACCEL_* */
    int ime_shift_toggle;           /* a Shift tap toggles the input method */
    int ime_ctrl_space;             /* Ctrl+Space and Super+Space toggle it */
    int display_follow;             /* the mode follows the size requests of the host display */
};
#define POINTER_ACCEL_FLAT 0
#define POINTER_ACCEL_ADAPTIVE 1
/* display_mode packs width, height (up to 16383) and the pixel scale (1..4). */
extern struct comp_settings settings;
void debug_init(struct wire_server *srv);
void debug_screen_changed(void);              /* sends the new size to screencopy clients */
void debug_setting_changed(const char *key, int value);   /* sends a value to every settings client */
/* trace.c: the tracer global, and the client events of a running trace
 * (connected 1 when a client appears or reports its pid, 0 when it goes). */
void trace_init(struct wire_server *srv);
void trace_client(const struct client *c, int connected);
void frame_clock_set(int ms);                   /* main.c */
/* surface.c: whether a surface waits for a frame callback, and the
 * completion of the callbacks after a frame. */
int surfaces_frame_pending(void);
void surfaces_frame_done(uint32_t time_ms);
/* stats.c: the frame statistics (docs/design/graphics-performance.md).
 * The events start the latencies that the next frame ends. */
enum stats_event { STATS_DAMAGE, STATS_COMMIT, STATS_INPUT, STATS_EVENTS };
void stats_reset(void);
void stats_wakeup(void);                        /* one return of poll */
void stats_idle_timer(void);                    /* a frame timer expiration without damage */
void stats_mark(enum stats_event e);
long stats_frame_begin(void);                   /* returns the start time for stats_frame_end */
void stats_rect(long device_pixels);            /* one composed damage rectangle */
void stats_flush(long rects, long bytes, long us);  /* one present of the backend */
long stats_frame_end(long t0);                  /* returns the frame time in microseconds */
void stats_cursor_move(long t0);                /* a device cursor move that started at t0 */
void stats_pool_mapped(long delta);             /* bytes of client pools mapped or unmapped */
void stats_values(long *count, long *ms, long *max);
void stats_send(struct wire_resource *r);
void stats_log(void);
void seat_repeat_changed(void);                 /* seat.c */
void seat_tick(long now);
/* The uptime in ms of the next key repeat, or -1 for none (G6). */
long seat_next_deadline(void);

/* main.c */
extern int screen_w, screen_h;
extern int screen_scale;                /* framebuffer pixels per logical pixel */
extern int cursor_x, cursor_y;              /* logical pixel under the cursor */
void comp_log(const char *fmt, ...);
/* Frequent lines (frames, keys, commits, releases), only when verbose. */
void comp_debug(const char *fmt, ...);
/* input.c: the devices under /dev/input, and the cursor position in
 * fractions of a logical pixel (cursor_x is its floor). */
extern double cursor_fx, cursor_fy;
int input_init(void);
struct pollfd;
int input_fill_pollfds(struct pollfd *pf, int max);
void input_handle(const struct pollfd *pf, int n);
void input_close(void);
/* Place the cursor at the centre of a logical pixel. */
void input_place_cursor(int x, int y);
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
struct csurface *scene_surface_at(int x, int y);           /* content hit, topmost */
int scene_order(struct csurface **out, int max);           /* bottom to top */
/* Screen capture: the pointer's rectangle in device pixels (empty when
 * hidden), the last frame with or without the pointer into a buffer of
 * the back buffer's size, the extent of a toplevel with its shadow in
 * logical pixels, and the toplevel alone into an ARGB buffer of its
 * extent in device pixels (0 or -ENOMEM). stride is in bytes. */
struct rect scene_pointer_rect(void);
void scene_copy_screen(uint8_t *to, int stride, int pointer);
struct rect scene_window_extent(const struct toplevel *t);
int scene_render_window(struct toplevel *t, uint8_t *to, int stride);
/* backend_fb.c */
extern struct surface back;
int backend_init(void);
/* Show the composed rectangles (logical pixels, at most RECT_SET_MAX):
 * copy them from the back buffer into the framebuffer unless the scene
 * composes into the framebuffer itself, and flush them in one request. */
void backend_present(const struct rect *r, int n);
/* The bytes of the private back buffer. */
long backend_buffer_bytes(void);
int backend_can_set_mode(void);
/* Change the framebuffer mode; screen_w, screen_h, screen_scale and the
 * back buffer follow. */
int backend_set_mode(int width, int height, int scale);
/* The descriptor of /dev/fb0. It becomes readable when the host display
 * requests a size. */
int backend_display_fd(void);
/* Reads the last size request of the host display in device pixels.
 * Returns 0, or -1 when the host made no request. */
int backend_display_request(int *width, int *height);
/* The device cursor (FB_CAP_CURSOR, G9). backend_cursor_set shows the
 * image of w x h pixels (0xAARRGGBB, straight alpha, rows of stride
 * pixels) with the hotspot at (hot_x, hot_y) of the image over the device
 * pixel (x, y), or hides the cursor when w is 0. Both return 0 or -1. */
int backend_has_cursor(void);
int backend_cursor_set(const uint32_t *image, int w, int h, int stride, int hot_x, int hot_y, int x, int y);
int backend_cursor_move(int x, int y);
/* main.c: apply a mode to the backend, the shell and every client. */
int comp_set_mode(int width, int height, int scale);
/* Applies the last size request of the host display at the chosen scale
 * when display_follow is set. */
void comp_follow_display(void);
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
/* The visible frame of a toplevel on screen: the server decorations,
 * or the client's window geometry, or the surface. */
struct rect toplevel_frame(const struct toplevel *t);
/* The size a configure describes: the surface for server decorations,
 * the window geometry for client ones. */
void toplevel_configure_size(const struct toplevel *t, int *w, int *h);
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
void decor_init(void);                                     /* loads the title font */
struct rect decor_frame(const struct csurface *s);         /* frame including the title bar */
struct rect decor_extent(const struct csurface *s);        /* frame plus server-side shadow */
struct rect decor_opaque(const struct csurface *s);        /* part of the frame without rounded corners */
int decor_has(const struct csurface *s);
void decor_draw_shadow(struct csurface *s, struct rect clip);
void decor_draw(struct csurface *s, struct rect clip);
/* Free what decor.c stores for a toplevel that goes away. */
void decor_free(struct toplevel *t);
/* Returns 1 when the press at the cursor was consumed by decorations. */
int decor_press(struct csurface *s, int button);
int decor_motion(void);
int decor_release(void);                 /* 1: server drag ended, 2: client requested drag ended */
int decor_dragging(void);
/* seat.c */
void seat_init(struct wire_server *srv);
/* Load /usr/share/keymaps/<name>.mkm for the server and for clients bound from now on. */
int seat_load_keymap(const char *name);
void seat_pointer_motion(void);
void seat_pointer_button(int button, int pressed);
/* value: logical pixels in 24.8 fixed point, positive towards the user. */
void seat_pointer_axis(int value);
void seat_key(uint32_t key, int pressed);
void seat_set_keyboard_focus(struct csurface *s);
struct csurface *seat_keyboard_focus(void);
void seat_surface_gone(struct csurface *s);
uint32_t seat_last_serial(void);
int seat_modifiers(void);
int seat_buttons(void);                  /* pointer buttons pressed, bit 0 = left */
int seat_validate_serial(struct client *client, uint32_t serial);
int seat_validate_grab(struct client *client, struct csurface *origin, uint32_t serial);
int seat_validate_drag(struct client *client, struct csurface *origin, uint32_t serial);
void seat_drag_started(void);
void seat_drag_ended(int button_pressed);
struct csurface *seat_cursor_surface(void);
int seat_cursor_hidden(void);
int seat_translate(uint32_t key, int mods);
/* The keymap of the seat, for the compositions of dead keys. */
struct keymap;
const struct keymap *seat_keymap(void);
void seat_layout_label(char *out, size_t size);
void seat_input_label_changed(void);
int seat_keymap_fd(uint32_t *size);
void seat_deliver_key(uint32_t key, int pressed, int mods);

/* inputmethod.c: the methods of the seat and the relay to the input method
 * daemon (docs/design/ime.md). */
void im_init(struct wire_server *srv);
void im_label(char *out, size_t size);
void im_select(int index);                 /* a method, or -1 for the next */
void im_toggle(void);
void im_select_in_order(int index);         /* after the keys that wait for the daemon */
void im_toggle_in_order(void);
int im_japanese_key(uint32_t key);
void im_update(void);
void im_context_changed(void);
int im_filter_key(uint32_t key, int pressed, int mods);
int im_busy(void);
void im_tick(long now);
/* The uptime in ms at which a key sent to the input method times out, or
 * -1 for none (G6). */
long im_next_deadline(void);
void im_modifiers(int depressed, int locked, int group);
void im_keymap_changed(int fd, uint32_t size);
void im_cursor_changed(int x, int y, int width, int height);
int im_candidates_visible(void);
void im_place_candidates(void);
void im_candidates_committed(struct csurface *s, int first_map);
void im_surface_gone(struct csurface *s);
/* text.c */

void text_init(struct wire_server *srv);
void text_focus_changed(struct csurface *old, struct csurface *now);
int text_focus_active(void);
int text_key(uint32_t key, int pressed, int mods);   /* 1: a composition used the key */
/* The state of the text input context that has the keyboard focus: 0
 * without one.  Each pointer may be NULL. */
int text_focused_state(const char **text, uint32_t *cursor, uint32_t *anchor, uint32_t *hints, uint32_t *purpose);
/* The changes of the input method daemon for the focused context. */
void text_im_apply(const char *commit, const char *preedit, int begin, int end, uint32_t before, uint32_t after);
void text_surface_gone(struct csurface *s);
void text_client_gone(struct client *c);
/* data.c */
void data_init(struct wire_server *srv);
void data_keyboard_focus_changed(struct client *c);
void data_pointer_motion(void);
void data_pointer_release(void);
void data_drag_cancel(void);
void data_drag_modifiers(void);          /* Ctrl and Shift choose copy and move */
void data_icon_committed(struct csurface *s, int attach_x, int attach_y);
int data_dragging(void);
void data_surface_gone(struct csurface *s);
void data_client_gone(struct client *c);
int data_fetch_fd(void);                 /* -1 when no transfer is in progress */
void data_fetch_read(void);
