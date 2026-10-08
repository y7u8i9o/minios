#pragma once
/* Client side of the display protocol (M26): windows are toplevel
 * surfaces with double buffered shared memory; events are delivered as
 * struct wmsg records, carried over from the M17 design so the framework and
 * the applications are unchanged. */
#include <gui/gfx.h>
#include <minios/input.h>
#include <stdint.h>

#define WSRV_TITLE_MAX 48
#define WSRV_CLIP_MAX 65536
#define GUI_CLIP_MAX WSRV_CLIP_MAX

enum wmsg_type {
    WM_KEY = 36, WM_MOUSE, WM_FOCUS, WM_CLOSE, WM_RESIZED, WM_TEXT, WM_PREEDIT, WM_TEXT_DELETE,
    WM_DRAG_ENTER, WM_DRAG_MOTION, WM_DRAG_LEAVE, WM_DROP, WM_DRAG_END,
};

#define WMOUSE_MOVE  0
#define WMOUSE_DOWN  1
#define WMOUSE_UP    2
#define WMOUSE_WHEEL 3

#define WMOD_SHIFT 1
#define WMOD_CTRL  2
#define WMOD_ALT   4
#define WMOD_LOGO  8

/* WM_KEY: a = key code (the Linux codes KEY_* of minios/input.h),
 * b = 1 down / 0 up, c = modifiers, d = translated character or 0. A
 * pressed key repeats after the compositor's delay at its rate: the
 * library queues further WM_KEY down messages until the release.
 * WM_MOUSE: a = x, b = y in window contents coordinates, c = buttons
 * (WMOUSE_WHEEL: the delta), d = kind. WM_FOCUS: a = 1 gained / 0 lost.
 * WM_RESIZED: a = width, b = height (the surface is already resized).
 * WM_TEXT: text is a UTF-8 commit from the text-input protocol.
 * WM_DRAG_ENTER, WM_DRAG_MOTION: a drag is over the window; a = x, b = y
 * in contents coordinates, c = the action the compositor chose (GUI_DND_*,
 * 0 for none), d = the actions the source allows. The window answers with
 * gui_drag_accept. A change of the action alone repeats WM_DRAG_MOTION.
 * WM_DRAG_LEAVE: the drag left the window or was cancelled.
 * WM_DROP: the data of a drop is ready (gui_drop_data); a, b and c as
 * above. WM_DRAG_END goes to the window that started a drag: a = the
 * action performed, 0 when the drag was cancelled. */
struct wmsg {
    uint32_t type;
    int32_t pid;
    int32_t window;
    int32_t a, b, c, d;
    char text[WSRV_TITLE_MAX];
};

struct gui_window {
    int id;
    int width, height;          /* logical pixels */
    int scale;                  /* device pixels per logical pixel (the output's scale) */
    /* width * scale by height * scale device pixels in the shared buffer:
     * call gui_begin_paint, draw here, then gui_damage. */
    struct surface surf;
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
/* The caret of the focused text widget in window coordinates, where the
 * compositor shows the candidates of an input method. */
void gui_text_input_set_cursor(struct gui_window *window, int x, int y, int width, int height);
/* A layer surface without decorations. layer is 0 for the background, 1
 * for the bottom, 2 for the top and 3 for the overlay layer. anchor is a
 * mask of GUI_ANCHOR_* edges. A dimension of 0 selects the free desktop
 * area. keyboard 1 requests key events. The surface is mapped by the
 * first commit after the caller has drawn into it (gui_flush or
 * gui_next_event). */
#define GUI_ANCHOR_TOP 1
#define GUI_ANCHOR_BOTTOM 2
#define GUI_ANCHOR_LEFT 4
#define GUI_ANCHOR_RIGHT 8
struct gui_window *gui_create_layer_window(int width, int height, int layer, int anchor, int exclusive,
                                           int keyboard, const char *ns);
/* Set the margins of a layer window from the anchored edges of the
 * desktop area (the screen without the panel). The margins take effect
 * with the next commit. */
void gui_layer_set_margin(struct gui_window *w, int top, int right, int bottom, int left);
/* The session lock (protocol/lock.xml, docs/design/lock.md). Lock the
 * session and create a window on the lock surface, which covers the
 * screen and receives all input. Return NULL when X12 refuses the lock.
 * The window closes when the session ends. Destroying the window without
 * gui_unlock_session leaves the session locked. */
struct gui_window *gui_create_lock_window(void);
/* Unlock the session of a lock window. Return 0, or -1 when the window
 * has no active lock. */
int gui_unlock_session(struct gui_window *w);
void gui_destroy_window(struct gui_window *w);
/* Mark a rectangle (logical pixels) changed; it is committed with the next frame. */
void gui_damage(struct gui_window *w, int x, int y, int width, int height);
/* Surface hints are committed atomically with the next buffer update. */
void gui_set_opaque_region(struct gui_window *w, const struct rect *rects, int count);
/* Give the window ARGB buffers without an opaque region. X12 copies the
 * pixels inside a later opaque region (gui_set_opaque_region) and blends
 * the other pixels with their alpha, as for a dimmed full screen overlay.
 * The colours of the theme have the alpha 0, so widgets outside the
 * opaque region are invisible. */
void gui_set_translucent(struct gui_window *w);
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
/* Drag and drop (docs/design/dnd.md). The actions are copy and move. */
#define GUI_DND_COPY 1
#define GUI_DND_MOVE 2
#define GUI_DND_MAX (16 << 20)      /* bytes of one drop; longer data is cut */
struct gui_drag_item {
    const char *mime;
    const void *data;
    size_t len;
};
/* Start a drag from window w while the mouse button pressed in it is
 * down. The library copies up to eight items, one per MIME type, and
 * serves them to the target. actions is a mask of GUI_DND_*. icon is the
 * drag image in device pixels at the window's scale (ARGB), or NULL;
 * (hot_x, hot_y) is the point of the icon under the cursor in logical
 * pixels. The window receives no release of that button: WM_DRAG_END
 * reports the end of the drag. Returns 0 or a negative errno. */
int gui_drag_start(struct gui_window *w, const struct gui_drag_item *items, int nitems, int actions,
                   const struct surface *icon, int hot_x, int hot_y);
/* 1 while a drag started by this process runs. */
int gui_dragging(void);
/* 1 when the drag over a window of this process offers the MIME type. */
int gui_drag_offers(const char *mime);
/* The answer to WM_DRAG_ENTER and WM_DRAG_MOTION: the type the window
 * takes on a drop, or NULL to refuse, the actions it supports and the one
 * it prefers. */
void gui_drag_accept(const char *mime, int actions, int preferred);
/* The data of the drag over a window of this process before the drop,
 * for a decision such as copy or move. The first call for a type starts
 * reading it and returns NULL; WM_DRAG_MOTION follows when it has arrived,
 * and later calls return it, terminated by a NUL byte, until the drag
 * leaves the window. */
const char *gui_drag_peek(const char *mime, size_t *len);
/* The data of the last WM_DROP, terminated by a NUL byte that len does
 * not count, valid until the next drop. */
const char *gui_drop_data(size_t *len, const char **mime);
/* The descriptor of drag data being read, or -1, for callers with their
 * own poll loop; they call gui_next_event(ev, 0) when it is readable. */
int gui_transfer_fd(void);
/* Wait for the next event; timeout_ms < 0 blocks. Returns 1 with the
 * event filled in, 0 on timeout, -1 on error. Pending damage is
 * committed before waiting. */
int gui_next_event(struct wmsg *ev, int timeout_ms);
/* Descriptor of the connection, for callers with their own poll loop. */
int gui_event_fd(void);
/* Milliseconds until the next key repeat is due, or -1 when no key is
 * pressed; callers with their own poll loop cap their timeout with it and
 * call gui_next_event(ev, 0) afterwards. */
int gui_repeat_timeout(void);
/* The modifier keys that are pressed now (WMOD_*), for mouse events. */
int gui_modifiers(void);
/* The longest time between two presses of a double click. */
#define GUI_DOUBLE_CLICK_MS 400
/* Commit pending damage now (done by gui_next_event as well). */
void gui_flush(void);
/* Prepare w->surf for drawing (G8 of docs/plan/compositor-performance.md).
 * The surface lies in a buffer that the compositor reads after a commit,
 * so a program calls this before it draws. The call moves to a free
 * buffer, copies the regions that changed meanwhile, and may wait up to
 * 100 ms for the compositor to release a buffer. */
void gui_begin_paint(struct gui_window *w);
/* Whether the last commit of w waits for its frame callback. The
 * framework paints a window only when it does not. */
int gui_frame_pending(const struct gui_window *w);
/* The rendering statistics of the windows of this process since its
 * start (docs/design/graphics-performance.md). Times are microseconds. */
struct gui_stats {
    uint64_t paints;            /* paints of a window that changed pixels */
    uint64_t paint_us;
    uint64_t commits;
    uint64_t copy_us;           /* copies into the shared buffers at a commit */
    uint64_t copied_bytes;
    uint64_t frame_waits;       /* commits deferred to a frame callback or a buffer release */
    uint64_t pool_bytes;        /* bytes of the shared buffer pools mapped now */
};
void gui_get_stats(struct gui_stats *out);
/* Count one paint of a window that took us microseconds (the framework). */
void gui_count_paint(long us);
struct wire_display;
struct wire_proxy;
struct wire_interface;
/* The libwire connection, and a bind of any advertised global by
 * interface name (NULL when the compositor has none, or only a lower
 * version). */
struct wire_display *gui_display(void);
struct wire_proxy *gui_bind_global(const char *iface, const struct wire_interface *interface, int version);
