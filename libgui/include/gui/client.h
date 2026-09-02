#pragma once
/* Client side of the display protocol (M26): windows are toplevel
 * surfaces with double buffered shared memory; events are delivered as
 * struct wmsg records, kept from the M17 design so the framework and
 * the applications are unchanged. */
#include <gui/gfx.h>
#include <stdint.h>

#define WSRV_TITLE_MAX 48
#define WSRV_CLIP_MAX 65536
#define GUI_CLIP_MAX WSRV_CLIP_MAX

enum wmsg_type {
    WM_KEY = 36, WM_MOUSE, WM_FOCUS, WM_CLOSE, WM_RESIZED, WM_TEXT, WM_PREEDIT, WM_TEXT_DELETE,
};

#define WMOUSE_MOVE  0
#define WMOUSE_DOWN  1
#define WMOUSE_UP    2
#define WMOUSE_WHEEL 3

#define WMOD_SHIFT 1
#define WMOD_CTRL  2
#define WMOD_ALT   4

/* WM_KEY: a = key code (PS/2 set 1, 0x80 added for the 0xe0 prefix),
 * b = 1 down / 0 up, c = modifiers, d = translated character or 0.
 * WM_MOUSE: a = x, b = y in window contents coordinates, c = buttons
 * (WMOUSE_WHEEL: the delta), d = kind. WM_FOCUS: a = 1 gained / 0 lost.
 * WM_RESIZED: a = width, b = height (the surface is already resized).
 * WM_TEXT: text is a UTF-8 commit from the text-input protocol. */
struct wmsg {
    uint32_t type;
    int32_t pid;
    int32_t window;
    int32_t a, b, c, d;
    char text[WSRV_TITLE_MAX];
};

struct gui_window {
    int id;
    int width, height;
    struct surface surf;        /* draw here, then gui_damage */
    struct gui_window *next;
    void *priv;                 /* library state */
};

/* Connect to the compositor. Returns 0, or -1 with errno. */
int gui_connect(void);
void gui_disconnect(void);
int gui_screen_width(void);
int gui_screen_height(void);
struct gui_output_info {
    int x, y, width, height, scale, transform, refresh_hz;
};
int gui_output_count(void);
int gui_get_output(int index, struct gui_output_info *out);
struct gui_window *gui_create_window(int width, int height, const char *title);
/* A transient/modal toplevel and a compositor managed popup. */
struct gui_window *gui_create_dialog_window(struct gui_window *parent, int width, int height, const char *title);
struct gui_window *gui_create_popup_window(struct gui_window *parent, int x, int y, int width, int height, int grab);
int gui_has_popup_surfaces(void);
/* Enable the text-input protocol for the focused text widget. */
void gui_text_input_set(struct gui_window *window, int enabled);
/* A layer surface (no decorations): layer 0 background, 1 bottom, 2 top,
 * 3 overlay; anchor is a mask of GUI_ANCHOR_* edges; a dimension of 0
 * takes the free desktop area; keyboard 1 asks for key events. */
#define GUI_ANCHOR_TOP 1
#define GUI_ANCHOR_BOTTOM 2
#define GUI_ANCHOR_LEFT 4
#define GUI_ANCHOR_RIGHT 8
struct gui_window *gui_create_layer_window(int width, int height, int layer, int anchor, int exclusive,
                                           int keyboard, const char *ns);
void gui_destroy_window(struct gui_window *w);
/* Mark a rectangle changed; it is committed with the next frame. */
void gui_damage(struct gui_window *w, int x, int y, int width, int height);
/* Surface hints are committed atomically with the next buffer update. */
void gui_set_opaque_region(struct gui_window *w, const struct rect *rects, int count);
void gui_set_input_region(struct gui_window *w, const struct rect *rects, int count);
void gui_move(struct gui_window *w, int x, int y);
void gui_set_title(struct gui_window *w, const char *title);
/* Ask for a new size; WM_RESIZED follows (also after resizes started by
 * the user), with the surface already replaced. */
void gui_resize(struct gui_window *w, int width, int height);
void gui_set_min_size(struct gui_window *w, int width, int height);
/* Clipboard text. gui_clipboard_get returns the length copied into buf
 * (terminated when it fits), or -1. */
int gui_clipboard_set(const char *text, int len);
int gui_clipboard_get(char *buf, int size);
/* Wait for the next event; timeout_ms < 0 blocks. Returns 1 with the
 * event filled in, 0 on timeout, -1 on error. Pending damage is
 * committed before waiting. */
int gui_next_event(struct wmsg *ev, int timeout_ms);
/* Descriptor of the connection, for callers with their own poll loop. */
int gui_event_fd(void);
/* Commit pending damage now (done by gui_next_event as well). */
void gui_flush(void);
struct wire_display;
struct wire_proxy;
struct wire_interface;
/* The libwire connection, and a bind of any advertised global by
 * interface name (NULL when the compositor has none). */
struct wire_display *gui_display(void);
struct wire_proxy *gui_bind_global(const char *iface, const struct wire_interface *interface, int version);
