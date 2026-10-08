/* Window client over libwire: toplevel surfaces with two shared memory
 * buffers, damage committed once per compositor frame, input events
 * translated with the seat's keymap, the clipboard and drag and drop
 * through the data device. Toplevels carry client side decorations (csd.c): the drawing
 * surface contains the chrome around the contents, gui_window.surf is the
 * view of the contents, and the compositor is told the window geometry. */
#include <gui/client.h>
#include <gui/keymap.h>
#include "csd.h"
#include "buffers.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ipc.h>
#include <wire/client.h>
#include "core-client.h"
#include "shell-client.h"
#include "seat-client.h"
#include "data-client.h"
#include "text-client.h"
#include "lock-client.h"

#define QUEUE_MAX 256

/* A shared memory pool with GUI_SLOTS slots of cap bytes each. Untouched
 * slot memory takes no physical pages, so the third slot costs memory only
 * when a window uses it. */
struct pool {
    struct wire_proxy *pool;
    int fd;
    uint8_t *map;
    size_t size, cap;
    struct wire_proxy *proxy[GUI_SLOTS];    /* a buffer per slot, NULL before use */
    uint32_t format[GUI_SLOTS];
    int used[GUI_SLOTS];                    /* the slot has held contents */
    int busy[GUI_SLOTS];                    /* old pools only: the compositor may read the slot */
};

struct win {
    struct gui_window *w;
    struct wire_proxy *surface, *toplevel, *popup, *layer;
    struct pool pool;
    struct pool *old;           /* previous pools until the compositor releases their buffers */
    int nold;
    struct gui_buffers bufs;
    size_t content_off;         /* offset of gui_window.surf in a slot, in pixels */
    struct rect_set damage;     /* device pixels of the surface, since the last commit */
    int frame_pending, need_commit;
    int resize_failed;              /* the buffers do not match the size: commit nothing */
    int chrome_dirty;           /* 1 header bar, 2 the whole chrome, painted by gui_begin_paint */
    struct wire_proxy *frame_cb;    /* The frame callback that is pending, or NULL. */
    int min_w, min_h;
    int buttons;
    int px, py;                 /* last pointer position, surface coordinates */
    struct csd csd;
    struct wire_proxy *decoration;
    int press_zone;             /* zone of the current button press */
    uint32_t last_click;        /* release time of the previous header click (double click) */
    int32_t wheel_acc;          /* axis motion below one wheel click, 24.8 pixels */
    int translucent;            /* ARGB buffers without an opaque region (gui_set_translucent) */
    struct wire_proxy *session_lock, *lock_surface;
    int lock_state;             /* 1 after locked, -1 after finished, 0 before either */
};

/* Logical origin of the contents inside the surface. */
static void content_origin(struct gui_window *w, int *ox, int *oy)
{
    struct win *wi = w->priv;
    struct rect c = csd_content(&wi->csd, w->width, w->height);
    *ox = c.x;
    *oy = c.y;
}

static void add_damage(struct win *wi, struct rect r)
{
    r = rect_intersect(r, (struct rect){ 0, 0, wi->bufs.w, wi->bufs.h });
    if (!rect_empty(r))
        rect_set_add(&wi->damage, r);
}

/* Mark the chrome (or only the header bar) for repainting. Event handlers
 * run during a dispatch, when the current slot may be on screen, so the
 * painting waits for gui_begin_paint. */
static void chrome_repaint(struct gui_window *w, int header_only)
{
    struct win *wi = w->priv;
    if (!wi->csd.enabled || !w->surf.pixels)
        return;
    wi->chrome_dirty |= header_only ? 1 : 2;
}

static struct gui_stats stats;          /* gui_get_stats; the process draws from one thread */
static struct wire_display *display;
static struct wire_proxy *compositor, *shm, *shell, *seat, *pointer, *keyboard, *data_manager, *data_device;
static struct wire_proxy *text_manager, *text_input;
static struct gui_window *wins;
static int screen_w = 1024, screen_h = 768;
struct output_state {
    struct wire_proxy *proxy;
    uint32_t name;
    int active;
    struct gui_output_info info;
};
static struct output_state outputs[8];
static int noutputs;
static struct wmsg queue[QUEUE_MAX];
static int qhead, qtail;
static struct keymap *keymap;
static int modifiers;
static int group;                   /* the keymap group that the compositor reports */
static int pending_dead;            /* a dead key of a window without text input, or 0 */
static uint32_t last_serial;
static struct gui_window *pointer_win, *keyboard_win;
/* Key repeat from the seat's repeat_info: the pressed key and the time of
 * its next repeat (0: none). */
static int repeat_rate = 30, repeat_delay = 500;
static uint32_t repeat_key;
static long repeat_at;
static struct gui_window *text_win;
static int text_active;
static uint32_t text_serial;
static int text_cursor[4];          /* the caret rectangle sent last, surface coordinates */
static int next_id = 1;
static struct wire_proxy *selection_offer, *selection_source;
static char *clip_text;
static int clip_len;
static uint32_t button_serial;      /* of the last button press, for a drag */

static void push(const struct wmsg *m)
{
    int next = (qtail + 1) % QUEUE_MAX;
    if (next == qhead)
        return;
    queue[qtail] = *m;
    qtail = next;
}

static struct gui_window *window_of_surface(struct wire_proxy *s)
{
    for (struct gui_window *w = wins; w; w = w->next)
        if (((struct win *)w->priv)->surface == s)
            return w;
    return NULL;
}

static struct gui_window *window_of_toplevel(struct wire_proxy *t)
{
    for (struct gui_window *w = wins; w; w = w->next)
        if (((struct win *)w->priv)->toplevel == t)
            return w;
    return NULL;
}

/* ---- registry and seat ---- */

/* Every global seen, for gui_bind_global. */
static struct { uint32_t name; char iface[32]; uint32_t version; } globals[32];
static int nglobals;
static struct wire_proxy *registry_proxy;

static void output_bounds(void)
{
    if (!noutputs)
        return;
    int first = -1;
    for (int i = 0; i < 8; i++) if (outputs[i].active) { first = i; break; }
    if (first < 0) return;
    int x0 = outputs[first].info.x, y0 = outputs[first].info.y;
    int x1 = x0 + outputs[first].info.width, y1 = y0 + outputs[first].info.height;
    for (int i = first + 1; i < 8; i++) {
        if (!outputs[i].active) continue;
        if (outputs[i].info.x < x0) x0 = outputs[i].info.x;
        if (outputs[i].info.y < y0) y0 = outputs[i].info.y;
        if (outputs[i].info.x + outputs[i].info.width > x1) x1 = outputs[i].info.x + outputs[i].info.width;
        if (outputs[i].info.y + outputs[i].info.height > y1) y1 = outputs[i].info.y + outputs[i].info.height;
    }
    screen_w = x1 - x0;
    screen_h = y1 - y0;
}

static void on_global(void *user, struct wire_proxy *registry, uint32_t name, const char *iface, uint32_t version)
{
    if (nglobals < 32) {
        globals[nglobals].name = name;
        strlcpy(globals[nglobals].iface, iface, sizeof globals[0].iface);
        globals[nglobals].version = version;
        nglobals++;
    }
    if (strcmp(iface, "compositor") == 0) compositor = registry_bind(registry, name, iface, version, &compositor_interface, 1);
    else if (strcmp(iface, "shm") == 0) shm = registry_bind(registry, name, iface, version, &shm_interface, 1);
    else if (strcmp(iface, "shell") == 0) shell = registry_bind(registry, name, iface, version, &shell_interface, 1);
    else if (strcmp(iface, "seat") == 0) seat = registry_bind(registry, name, iface, version, &seat_interface, 1);
    else if (strcmp(iface, "data_device_manager") == 0) data_manager = registry_bind(registry, name, iface, version, &data_device_manager_interface, 1);
    else if (strcmp(iface, "text_input_manager") == 0) text_manager = registry_bind(registry, name, iface, version, &text_input_manager_interface, 1);
    else if (strcmp(iface, "output") == 0) {
        extern const struct output_listener output_events;
        if (noutputs < (int)(sizeof outputs / sizeof outputs[0])) {
            struct output_state *o = NULL;
            for (int i = 0; i < 8; i++) if (!outputs[i].active) { o = &outputs[i]; break; }
            if (!o) return;
            memset(o, 0, sizeof *o);
            o->active = 1;
            noutputs++;
            o->name = name;
            o->info.scale = 1;
            o->proxy = registry_bind(registry, name, iface, version, &output_interface, 1);
            output_add_listener(o->proxy, &output_events, o);
        }
    }
}
static void on_global_remove(void *user, struct wire_proxy *registry, uint32_t name)
{
    for (int i = 0; i < 8; i++)
        if (outputs[i].active && outputs[i].name == name) {
            wire_proxy_destroy(outputs[i].proxy);
            memset(&outputs[i], 0, sizeof outputs[i]);
            noutputs--;
            output_bounds();
            break;
        }
}
static const struct registry_listener registry_events = { on_global, on_global_remove };
static void on_geometry(void *user, struct wire_proxy *o, int32_t x, int32_t y, int32_t w, int32_t h)
{ struct output_state *s = user; s->info.x = x; s->info.y = y; s->info.width = w; s->info.height = h; }
static void on_mode(void *user, struct wire_proxy *o, int32_t w, int32_t h, int32_t r)
{ struct output_state *s = user; s->info.width = w; s->info.height = h; s->info.refresh_hz = r; }
static void on_output_scale(void *user, struct wire_proxy *o, int32_t factor)
{ ((struct output_state *)user)->info.scale = factor > 0 ? factor : 1; }
static void on_output_transform(void *user, struct wire_proxy *o, uint32_t transform)
{ ((struct output_state *)user)->info.transform = (int)transform; }
/* The scale of the output windows live on; windows are re-created at
 * the new scale when it changes. */
static int output_scale(void)
{
    for (int i = 0; i < 8; i++)
        if (outputs[i].active)
            return outputs[i].info.scale > 0 ? outputs[i].info.scale : 1;
    return 1;
}
static void surface_resize(struct gui_window *w, int width, int height);
/* A new output scale is applied after the events of the same read. X12
 * sends the configure events of a mode change right after the output
 * events. A window with a configure among them is then resized once, at
 * its new size and the new scale, and not before at its old size and the
 * new scale, which could exceed the limit of a memory pool. */
static int scale_check;

static void on_output_done(void *user, struct wire_proxy *o)
{
    output_bounds();
    scale_check = 1;
}

/* Resizes the windows whose buffers have another scale than the output,
 * after the events of a read were dispatched. */
static void apply_output_scale(void)
{
    if (!scale_check)
        return;
    scale_check = 0;
    int s = output_scale();
    for (struct gui_window *w = wins; w; w = w->next) {
        if (w->scale == s || !w->surf.pixels)
            continue;
        surface_resize(w, w->width, w->height);
        struct wmsg m = { WM_RESIZED, 0, w->id, w->width, w->height, 0, 0, "" };
        push(&m);
    }
}

/* Dispatches the events of one read and then applies a new scale. */
static int dispatch_events(void)
{
    int r = wire_display_dispatch(display);
    apply_output_scale();
    return r;
}
const struct output_listener output_events = { on_geometry, on_mode, on_output_scale, on_output_transform, on_output_done };

/* The zone under the pointer, with the button hover state maintained up to
 * date; a press retains its zone until the release. */
static enum csd_zone pointer_zone(struct gui_window *w, int *edges)
{
    struct win *wi = w->priv;
    enum csd_zone z = csd_hit(&wi->csd, w->width, w->height, wi->px, wi->py, edges);
    int hover = z == CSD_CLOSE || z == CSD_MAXIMIZE || z == CSD_MINIMIZE ? (int)z : 0;
    if (hover != wi->csd.hover) {
        wi->csd.hover = hover;
        chrome_repaint(w, 1);
    }
    return z;
}

/* A pointer message to the application in contents coordinates. */
static void push_mouse(struct gui_window *w, int c, int kind)
{
    struct win *wi = w->priv;
    int ox, oy;
    content_origin(w, &ox, &oy);
    struct wmsg m = { WM_MOUSE, 0, w->id, wi->px - ox, wi->py - oy, c, kind, "" };
    push(&m);
}

static void on_ptr_enter(void *user, struct wire_proxy *p, uint32_t serial, struct wire_proxy *s, int32_t x, int32_t y)
{
    last_serial = serial;
    pointer_win = window_of_surface(s);
    if (pointer_win) {
        struct win *wi = pointer_win->priv;
        wi->px = wire_fixed_to_int(x);
        wi->py = wire_fixed_to_int(y);
        int edges;
        if (pointer_zone(pointer_win, &edges) == CSD_CONTENT)
            push_mouse(pointer_win, wi->buttons, WMOUSE_MOVE);
    }
}
static void on_ptr_leave(void *user, struct wire_proxy *p, uint32_t serial, struct wire_proxy *s)
{
    struct gui_window *w = window_of_surface(s);
    if (w) {
        struct win *wi = w->priv;
        if (wi->csd.hover) {
            wi->csd.hover = 0;
            chrome_repaint(w, 1);
        }
    }
    if (pointer_win == w)
        pointer_win = NULL;
}
static void on_ptr_motion(void *user, struct wire_proxy *p, uint32_t time, int32_t x, int32_t y)
{
    if (!pointer_win)
        return;
    struct win *wi = pointer_win->priv;
    wi->px = wire_fixed_to_int(x);
    wi->py = wire_fixed_to_int(y);
    int edges;
    enum csd_zone z = pointer_zone(pointer_win, &edges);
    if (wi->buttons ? wi->press_zone == CSD_CONTENT : z == CSD_CONTENT)
        push_mouse(pointer_win, wi->buttons, WMOUSE_MOVE);
}
static void drag_end(int action);
static struct wire_proxy *drag_source;

static void on_ptr_button(void *user, struct wire_proxy *p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    last_serial = serial;
    if (state)
        button_serial = serial;
    else if (drag_source)
        drag_end(0);            /* the compositor refused the drag: it delivers the release */
    if (!pointer_win)
        return;
    struct gui_window *w = pointer_win;
    struct win *wi = w->priv;
    int bit = 1 << (button - 1), edges;
    int first = wi->buttons == 0;
    wi->buttons = state ? wi->buttons | bit : wi->buttons & ~bit;
    enum csd_zone z = pointer_zone(w, &edges);
    if (state && first)
        wi->press_zone = (int)z;
    if (wi->press_zone == CSD_CONTENT) {
        push_mouse(w, wi->buttons, state ? WMOUSE_DOWN : WMOUSE_UP);
        return;
    }
    if (button != 1)
        return;
    if (state) {
        switch (wi->press_zone) {
        case CSD_HEADER_BAR:
            /* A second press within GUI_DOUBLE_CLICK_MS of a completed click toggles
             * maximized; a press repeated during a drag does not. */
            if (wi->last_click && time - wi->last_click < GUI_DOUBLE_CLICK_MS && wi->toplevel) {
                if (wi->csd.maximized) toplevel_unset_maximized(wi->toplevel);
                else toplevel_set_maximized(wi->toplevel);
                wi->last_click = 0;
            } else if (wi->toplevel && !wi->csd.maximized) {
                toplevel_move(wi->toplevel, seat, serial);
            }
            break;
        case CSD_RESIZE:
            if (wi->toplevel)
                toplevel_resize(wi->toplevel, seat, serial, (uint32_t)edges);
            break;
        default:
            break;
        }
    } else if ((int)z == wi->press_zone && wi->toplevel) {
        /* The buttons act on release over the same button. */
        if (z == CSD_HEADER_BAR) {
            wi->last_click = time;
        } else if (z == CSD_CLOSE) {
            struct wmsg m = { WM_CLOSE, 0, w->id, 0, 0, 0, 0, "" };
            push(&m);
        } else if (z == CSD_MAXIMIZE) {
            if (wi->csd.maximized) toplevel_unset_maximized(wi->toplevel);
            else toplevel_set_maximized(wi->toplevel);
        } else if (z == CSD_MINIMIZE) {
            toplevel_set_minimized(wi->toplevel);
        }
    }
}
static void on_ptr_axis(void *user, struct wire_proxy *p, uint32_t time, uint32_t axis, int32_t value)
{
    if (!pointer_win)
        return;
    struct win *wi = pointer_win->priv;
    int edges;
    if (csd_hit(&wi->csd, pointer_win->width, pointer_win->height, wi->px, wi->py, &edges) != CSD_CONTENT)
        return;
    /* One wheel click is 15 logical pixels; finer motion accumulates
     * until it reaches a click. */
    wi->wheel_acc += value;
    int clicks = wi->wheel_acc / (15 * 256);
    if (clicks) {
        wi->wheel_acc -= clicks * 15 * 256;
        push_mouse(pointer_win, clicks, WMOUSE_WHEEL);
    }
}
static void on_ptr_frame(void *user, struct wire_proxy *p) {}
static const struct pointer_listener pointer_events = { on_ptr_enter, on_ptr_leave, on_ptr_motion, on_ptr_button, on_ptr_axis, on_ptr_frame };

static void on_keymap(void *user, struct wire_proxy *k, uint32_t format, int fd, uint32_t size)
{
    keymap_free(keymap);
    keymap = keymap_from_fd(fd, size);
    close(fd);
}
static void on_kbd_enter(void *user, struct wire_proxy *k, uint32_t serial, struct wire_proxy *s, const struct wire_array *keys)
{
    last_serial = serial;
    keyboard_win = window_of_surface(s);
    if (keyboard_win) {
        struct wmsg m = { WM_FOCUS, 0, keyboard_win->id, 1, 0, 0, 0, "" };
        push(&m);
    }
}
static void on_kbd_leave(void *user, struct wire_proxy *k, uint32_t serial, struct wire_proxy *s)
{
    repeat_at = 0;
    struct gui_window *w = window_of_surface(s);
    if (w) {
        struct wmsg m = { WM_FOCUS, 0, w->id, 0, 0, 0, 0, "" };
        push(&m);
    }
    if (keyboard_win == w)
        keyboard_win = NULL;
}
/* push_key translates a key with the group and the locked Caps Lock that
 * the compositor reports.  In a window without text input a dead key waits
 * for the next character and composes with it.  With text input the
 * compositor composes, and the characters of key events are dropped.
 * Widgets receive the modifiers Shift, Ctrl, Alt and Logo only. */
static void push_key(uint32_t key, int down)
{
    int ch = keymap_translate_group(keymap, key, modifiers, group);
    if (down && !text_active && keysym_is_dead(ch)) {
        pending_dead = ch;
        ch = 0;
    } else if (down && pending_dead && ch >= 32 && !keysym_is_symbol(ch) && !(modifiers & (WMOD_CTRL | WMOD_ALT))) {
        int composed = keymap_compose(keymap, pending_dead, ch);
        if (composed)
            ch = composed;
        pending_dead = 0;
    }
    if (keysym_is_symbol(ch))
        ch = 0;
    if (text_active && ch >= 32 && !(modifiers & (WMOD_CTRL | WMOD_ALT)))
        ch = 0;
    int mods = modifiers & (WMOD_SHIFT | WMOD_CTRL | WMOD_ALT | WMOD_LOGO);
    struct wmsg m = { WM_KEY, 0, keyboard_win->id, (int32_t)key, down, mods, ch, "" };
    push(&m);
}

static int is_modifier_key(uint32_t key)
{
    switch (key) {
    case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT: case KEY_LEFTCTRL: case KEY_RIGHTCTRL:
    case KEY_LEFTALT: case KEY_RIGHTALT: case KEY_LEFTMETA: case KEY_RIGHTMETA: case KEY_CAPSLOCK:
        return 1;
    }
    return 0;
}

static void on_key(void *user, struct wire_proxy *k, uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
    last_serial = serial;
    /* X12 composes the text of a window with a text input context, and
     * X12 repeats its keys (input.md). libgui repeats the other keys. */
    if (state && !is_modifier_key(key)) {
        repeat_at = 0;
        if (repeat_rate > 0 && !text_active) {
            repeat_key = key;
            repeat_at = uptime_ms() + repeat_delay;
        }
    } else if (key == repeat_key) {
        repeat_at = 0;
    }
    if (!keyboard_win)
        return;
    push_key(key, state ? 1 : 0);
}
static void on_modifiers(void *user, struct wire_proxy *k, uint32_t serial, uint32_t dep, uint32_t lat, uint32_t lock,
                         uint32_t grp)
{
    modifiers = (int)dep | (int)(lock & KEYMAP_MOD_CAPS);
    group = (int)grp;
}

int gui_modifiers(void)
{
    return modifiers;
}
static void on_repeat(void *user, struct wire_proxy *k, int32_t rate, int32_t delay)
{
    repeat_rate = rate;
    repeat_delay = delay;
    repeat_at = 0;
}

/* Queue the repeat of the pressed key when its time has come. */
static void repeat_tick(void)
{
    if (!repeat_at || !keyboard_win)
        return;
    long now = uptime_ms();
    if (now < repeat_at)
        return;
    push_key(repeat_key, 1);
    int period = repeat_rate > 0 ? 1000 / repeat_rate : 0;
    if (period < 1)
        period = 1;
    repeat_at += period;
    if (repeat_at <= now)
        repeat_at = now + period;
}

int gui_repeat_timeout(void)
{
    if (!repeat_at || !keyboard_win)
        return -1;
    long d = repeat_at - uptime_ms();
    return d < 0 ? 0 : (int)d;
}
static const struct keyboard_listener keyboard_events = { on_keymap, on_kbd_enter, on_kbd_leave, on_key, on_modifiers, on_repeat };

/* ---- text input ---- */

static void on_text_enter(void *user, struct wire_proxy *ti, struct wire_proxy *surface)
{
    text_win = window_of_surface(surface);
    text_active = text_win != NULL;
    if (text_active)
        repeat_at = 0;
}

static void on_text_leave(void *user, struct wire_proxy *ti, struct wire_proxy *surface)
{
    struct gui_window *w = window_of_surface(surface);
    if (text_win == w) {
        text_win = NULL;
        text_active = 0;
    }
}

static void push_text(uint32_t type, const char *text, int a, int b)
{
    if (!text_win)
        return;
    struct wmsg m = { type, 0, text_win->id, a, b, 0, 0, "" };
    strlcpy(m.text, text ? text : "", sizeof m.text);
    push(&m);
}

static void on_preedit(void *user, struct wire_proxy *ti, const char *text, int32_t begin, int32_t end)
{ push_text(WM_PREEDIT, text, begin, end); }
static void on_commit_string(void *user, struct wire_proxy *ti, const char *text)
{ push_text(WM_TEXT, text, 0, 0); }
static void on_delete_surrounding(void *user, struct wire_proxy *ti, uint32_t before, uint32_t after)
{ push_text(WM_TEXT_DELETE, "", (int)before, (int)after); }
static void on_text_done(void *user, struct wire_proxy *ti, uint32_t serial) {}
static const struct text_input_listener text_events = {
    on_text_enter, on_text_leave, on_preedit, on_commit_string, on_delete_surrounding, on_text_done,
};

/* ---- data device: clipboard and drag and drop ---- */

static void write_all(int fd, const char *p, size_t n)
{
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k > 0) {
            p += k;
            n -= (size_t)k;
        } else if (k < 0 && errno == EAGAIN) {
            struct pollfd pf = { fd, POLLOUT, 0 };
            if (poll(&pf, 1, 2000) <= 0)
                return;
        } else if (k < 0 && errno == EINTR) {
            continue;
        } else {
            return;
        }
    }
}

static void on_src_send(void *user, struct wire_proxy *s, const char *mime, int fd)
{
    if (clip_text)
        write_all(fd, clip_text, (size_t)clip_len);
    close(fd);
}
static void on_src_cancelled(void *user, struct wire_proxy *s)
{
    if (selection_source == s)
        selection_source = NULL;
    wire_proxy_destroy(s);
}
static const struct data_source_listener source_events = { on_src_send, on_src_cancelled, NULL, NULL, NULL };

/* The drag started by this process: the items it serves, the window it
 * started from, the action the compositor chose last and the icon. */
static struct {
    struct gui_drag_item items[8];      /* the data is owned */
    int nitems;
    int window;
    int action;
    struct wire_proxy *icon, *icon_pool, *icon_buf;
    int icon_fd;
    void *icon_map;
    size_t icon_size;
} drag = { .icon_fd = -1 };

static const struct gui_drag_item *drag_item(const char *mime)
{
    for (int i = 0; i < drag.nitems; i++)
        if (strcmp(drag.items[i].mime, mime) == 0)
            return &drag.items[i];
    return NULL;
}

static void drag_end(int action)
{
    if (!drag_source)
        return;
    struct wmsg m = { WM_DRAG_END, 0, drag.window, action, 0, 0, 0, "" };
    push(&m);
    data_source_destroy(drag_source);
    drag_source = NULL;
    for (int i = 0; i < drag.nitems; i++) {
        free((void *)drag.items[i].mime);
        free((void *)drag.items[i].data);
    }
    if (drag.icon_buf)
        buffer_destroy(drag.icon_buf);
    if (drag.icon_pool)
        shm_pool_destroy(drag.icon_pool);
    if (drag.icon)
        surface_destroy(drag.icon);
    if (drag.icon_map)
        munmap(drag.icon_map, drag.icon_size);
    if (drag.icon_fd >= 0)
        close(drag.icon_fd);
    memset(&drag, 0, sizeof drag);
    drag.icon_fd = -1;
}

static void on_drag_send(void *user, struct wire_proxy *s, const char *mime, int fd)
{
    const struct gui_drag_item *it = drag_item(mime);
    if (it)
        write_all(fd, it->data, it->len);
    close(fd);
}
static void on_drag_cancelled(void *user, struct wire_proxy *s) { drag_end(0); }
static void on_drag_dropped(void *user, struct wire_proxy *s) {}
/* The target has read the data. */
static void on_drag_finished(void *user, struct wire_proxy *s) { drag_end(drag.action ? drag.action : GUI_DND_COPY); }
static void on_drag_action(void *user, struct wire_proxy *s, uint32_t action) { drag.action = (int)action; }
static const struct data_source_listener drag_source_events = {
    on_drag_send, on_drag_cancelled, on_drag_dropped, on_drag_finished, on_drag_action,
};

/* What an offer announced, stored as the user data of its proxy, with
 * the data read before a drop (gui_drag_peek). */
struct offer_info {
    char mimes[8][64];
    int nmimes;
    uint32_t source_actions, action;
    char peek_mime[64];             /* requested; empty when none */
    char *peek;                     /* NULL until all of it arrived */
    size_t peek_len;
};

static struct offer_info *offer_info(struct wire_proxy *o) { return o ? wire_proxy_get_user_data(o) : NULL; }

static void peek_stop(void);

static void offer_free(struct wire_proxy *o)
{
    if (!o)
        return;
    struct offer_info *i = offer_info(o);
    if (i)
        free(i->peek);
    free(i);
    data_offer_destroy(o);
}

/* The drag over a window of this process, and the drop being read. */
static struct wire_proxy *target_offer;
static uint32_t target_serial;
static struct gui_window *target_win;
static int target_x, target_y;              /* contents coordinates */
static char target_mime[64];                /* accepted; empty while refused */
static int target_actions = -1, target_preferred = -1;
/* A read through a pipe: the drop (transfer) or the data of the drag
 * before the drop (peek); at most one of them runs at a time. */
struct pipe_read {
    int fd;                                 /* -1 when idle */
    struct wire_proxy *offer;
    char *buf;
    size_t len, cap;
    char mime[64];
    int window, x, y, action;
};
static struct pipe_read transfer = { .fd = -1 }, peek = { .fd = -1 };
static char *drop_buf;
static size_t drop_len;
static char drop_mime[64];

static void target_reset(void)
{
    target_offer = NULL;
    target_win = NULL;
    target_mime[0] = '\0';
    target_actions = target_preferred = -1;
}

static void push_drag(uint32_t type, struct gui_window *w, int x, int y, int action, int actions)
{
    struct wmsg m = { type, 0, w->id, x, y, action, actions, "" };
    push(&m);
}

static void on_offer_mime(void *user, struct wire_proxy *o, const char *mime)
{
    struct offer_info *i = user;
    if (i && i->nmimes < 8)
        strlcpy(i->mimes[i->nmimes++], mime, sizeof i->mimes[0]);
}
static void on_offer_source_actions(void *user, struct wire_proxy *o, uint32_t actions)
{
    struct offer_info *i = user;
    if (i)
        i->source_actions = actions;
}
static void on_offer_action(void *user, struct wire_proxy *o, uint32_t action)
{
    struct offer_info *i = user;
    if (!i)
        return;
    i->action = action;
    if (o == target_offer && target_win)
        push_drag(WM_DRAG_MOTION, target_win, target_x, target_y, (int)action, (int)i->source_actions);
}
static const struct data_offer_listener offer_events = { on_offer_mime, on_offer_source_actions, on_offer_action };

static void on_dev_offer(void *user, struct wire_proxy *dev, struct wire_proxy *offer)
{
    struct offer_info *i = calloc(1, sizeof *i);
    if (i)
        i->source_actions = GUI_DND_COPY;
    offer->obj.interface = &data_offer_interface;
    data_offer_add_listener(offer, &offer_events, i);
}

static void target_position(int32_t x, int32_t y)
{
    int ox, oy;
    content_origin(target_win, &ox, &oy);
    target_x = wire_fixed_to_int(x) - ox;
    target_y = wire_fixed_to_int(y) - oy;
}

static void on_dev_enter(void *user, struct wire_proxy *dev, uint32_t serial, struct wire_proxy *s, int32_t x, int32_t y, struct wire_proxy *offer)
{
    peek_stop();
    if (target_offer && target_offer != transfer.offer)
        offer_free(target_offer);
    target_reset();
    if (!offer)
        return;
    target_offer = offer;
    target_serial = serial;
    target_win = window_of_surface(s);
    if (!target_win) {
        data_offer_accept(offer, serial, NULL);
        return;
    }
    target_position(x, y);
    struct offer_info *i = offer_info(offer);
    push_drag(WM_DRAG_ENTER, target_win, target_x, target_y, i ? (int)i->action : 0, i ? (int)i->source_actions : 0);
}
static void on_dev_leave(void *user, struct wire_proxy *dev)
{
    peek_stop();
    if (target_win)
        push_drag(WM_DRAG_LEAVE, target_win, target_x, target_y, 0, 0);
    if (target_offer && target_offer != transfer.offer)
        offer_free(target_offer);
    target_reset();
}
static void on_dev_motion(void *user, struct wire_proxy *dev, uint32_t time, int32_t x, int32_t y)
{
    if (!target_win)
        return;
    target_position(x, y);
    struct offer_info *i = offer_info(target_offer);
    push_drag(WM_DRAG_MOTION, target_win, target_x, target_y, i ? (int)i->action : 0, i ? (int)i->source_actions : 0);
}

static void drop_deliver(char *buf, size_t len, const char *mime, int window, int x, int y, int action)
{
    free(drop_buf);
    drop_buf = buf;
    drop_len = len;
    strlcpy(drop_mime, mime, sizeof drop_mime);
    struct wmsg m = { WM_DROP, 0, window, x, y, action, 0, "" };
    push(&m);
}

/* Read what the source has written. Returns 1 at the end of the data,
 * which is then terminated and cut at GUI_DND_MAX, and 0 while more may
 * come. */
static int pipe_pump(struct pipe_read *r)
{
    for (;;) {
        if (r->len + 1 >= r->cap) {
            if (r->cap > GUI_DND_MAX)
                break;
            size_t cap = r->cap ? r->cap * 2 : 4096;
            char *b = realloc(r->buf, cap);
            if (!b)
                break;
            r->buf = b;
            r->cap = cap;
        }
        ssize_t k = read(r->fd, r->buf + r->len, r->cap - r->len - 1);
        if (k > 0) {
            r->len += (size_t)k;
            continue;
        }
        if (k < 0 && (errno == EAGAIN || errno == EINTR))
            return 0;
        break;
    }
    close(r->fd);
    r->fd = -1;
    if (r->len > GUI_DND_MAX)
        r->len = GUI_DND_MAX;
    if (r->buf)
        r->buf[r->len] = '\0';
    return 1;
}

static int pipe_start(struct pipe_read *r, struct wire_proxy *offer, const char *mime)
{
    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0)
        return -1;
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    r->fd = p[0];
    r->offer = offer;
    r->buf = NULL;
    r->len = r->cap = 0;
    strlcpy(r->mime, mime, sizeof r->mime);
    data_offer_receive(offer, mime, p[1]);
    close(p[1]);
    wire_display_flush(display);
    return 0;
}

static void peek_stop(void)
{
    if (peek.fd >= 0)
        close(peek.fd);
    free(peek.buf);
    memset(&peek, 0, sizeof peek);
    peek.fd = -1;
}

/* The data of the drag has arrived: the window sees the drag again. */
static void peek_pump(void)
{
    if (peek.fd < 0 || !pipe_pump(&peek))
        return;
    struct offer_info *i = offer_info(peek.offer);
    if (i && peek.offer == target_offer) {
        free(i->peek);
        i->peek = peek.buf ? peek.buf : strdup("");
        i->peek_len = peek.buf ? peek.len : 0;
        peek.buf = NULL;
        if (target_win)
            push_drag(WM_DRAG_MOTION, target_win, target_x, target_y, (int)i->action, (int)i->source_actions);
    }
    peek_stop();
}

/* Read the drop; at the end of the data, or at once with stop set, the
 * offer is finished and WM_DROP is queued. */
static void transfer_pump(int stop)
{
    if (transfer.fd < 0)
        return;
    if (stop) {
        close(transfer.fd);
        transfer.fd = -1;
        if (transfer.buf)
            transfer.buf[transfer.len] = '\0';
    } else if (!pipe_pump(&transfer)) {
        return;
    }
    data_offer_finish(transfer.offer);
    offer_free(transfer.offer);
    transfer.offer = NULL;
    drop_deliver(transfer.buf, transfer.len, transfer.mime, transfer.window, transfer.x, transfer.y, transfer.action);
    transfer.buf = NULL;
    transfer.len = transfer.cap = 0;
    wire_display_flush(display);
}

static void on_dev_drop(void *user, struct wire_proxy *dev)
{
    struct wire_proxy *offer = target_offer;
    struct gui_window *w = target_win;
    if (!offer || !w || !target_mime[0]) {
        on_dev_leave(user, dev);
        return;
    }
    struct offer_info *i = offer_info(offer);
    int action = i && i->action ? (int)i->action : GUI_DND_COPY;
    int x = target_x, y = target_y;
    char mime[64];
    strlcpy(mime, target_mime, sizeof mime);
    target_reset();
    /* A drag of this process: the compositor would route the data back to
     * this process, which reads it here; copy it directly. */
    if (drag_source) {
        const struct gui_drag_item *it = drag_item(mime);
        size_t len = it ? it->len : 0;
        char *buf = malloc(len + 1);
        if (buf) {
            if (len)
                memcpy(buf, it->data, len);
            buf[len] = '\0';
        }
        data_offer_finish(offer);
        offer_free(offer);
        drop_deliver(buf, buf ? len : 0, mime, w->id, x, y, action);
        return;
    }
    /* Data read before the drop is the drop's data. */
    if (i && i->peek && strcmp(i->peek_mime, mime) == 0) {
        char *buf = i->peek;
        size_t len = i->peek_len;
        i->peek = NULL;
        data_offer_finish(offer);
        offer_free(offer);
        drop_deliver(buf, len, mime, w->id, x, y, action);
        wire_display_flush(display);
        return;
    }
    peek_stop();
    transfer_pump(1);                   /* a drop that is still read ends with what arrived */
    if (pipe_start(&transfer, offer, mime) < 0) {
        offer_free(offer);
        return;
    }
    transfer.window = w->id;
    transfer.x = x;
    transfer.y = y;
    transfer.action = action;
}
static void on_dev_selection(void *user, struct wire_proxy *dev, struct wire_proxy *offer)
{
    if (selection_offer && selection_offer != offer)
        offer_free(selection_offer);
    selection_offer = offer;
}
static const struct data_device_listener device_events = { on_dev_offer, on_dev_enter, on_dev_leave, on_dev_motion, on_dev_drop, on_dev_selection };

/* ---- connection ---- */

/* The server's liveness ping; an answer that arrives late makes the
 * window show as not responding until it does. */
static void on_ping(void *user, struct wire_proxy *sh, uint32_t serial)
{
    shell_pong(sh, serial);
    wire_display_flush(display);
}
static const struct shell_listener shell_events = { on_ping };

int gui_connect(void)
{
    signal(SIGPIPE, SIG_IGN);           /* a vanished compositor is reported as an error, not a signal */
    display = wire_display_connect(NULL);
    if (!display)
        return -1;
    fcntl(wire_display_fd(display), F_SETFL, O_NONBLOCK);
    struct wire_proxy *registry = display_get_registry(wire_display_proxy(display));
    registry_proxy = registry;
    registry_add_listener(registry, &registry_events, NULL);
    /* The socket is non blocking: poll before each blocking dispatch. */
    for (int i = 0; i < 2; i++) {
        wire_display_flush(display);
        struct pollfd pf = { wire_display_fd(display), POLLIN, 0 };
        poll(&pf, 1, 2000);
        if (dispatch_events() < 0)
            return -1;
    }
    if (!compositor || !shm || !shell || !seat) {
        errno = ENOENT;
        return -1;
    }
    shell_add_listener(shell, &shell_events, NULL);
    shell_set_pid(shell, (uint32_t)getpid());
    pointer = seat_get_pointer(seat);
    pointer_add_listener(pointer, &pointer_events, NULL);
    keyboard = seat_get_keyboard(seat);
    keyboard_add_listener(keyboard, &keyboard_events, NULL);
    if (text_manager) {
        text_input = text_input_manager_get_text_input(text_manager, seat);
        text_input_add_listener(text_input, &text_events, NULL);
    }
    if (data_manager) {
        data_device = data_device_manager_get_data_device(data_manager, seat);
        data_device_add_listener(data_device, &device_events, NULL);
    }
    wire_display_flush(display);
    return 0;
}

void gui_disconnect(void)
{
    if (!display)
        return;
    drag_end(0);
    while (wins)
        gui_destroy_window(wins);
    wire_display_flush(display);
    wire_display_disconnect(display);
    display = NULL;
}

struct wire_display *gui_display(void) { return display; }

struct wire_proxy *gui_bind_global(const char *iface, const struct wire_interface *interface, int version)
{
    /* A bind above the advertised version is a protocol error that ends
     * the connection, so such a global counts as missing. */
    for (int i = 0; i < nglobals; i++)
        if (strcmp(globals[i].iface, iface) == 0 && globals[i].version >= (uint32_t)version)
            return registry_bind(registry_proxy, globals[i].name, iface, (uint32_t)version, interface, version);
    return NULL;
}

int gui_screen_width(void) { return screen_w; }
int gui_screen_height(void) { return screen_h; }
int gui_output_count(void) { return noutputs; }
int gui_get_output(int index, struct gui_output_info *out)
{
    if (index < 0 || index >= noutputs || !out)
        return -1;
    for (int i = 0; i < 8; i++)
        if (outputs[i].active && index-- == 0) {
            *out = outputs[i].info;
            return 0;
        }
    return -1;
}
int gui_event_fd(void) { return display ? wire_display_fd(display) : -1; }

/* ---- buffers ---- */

/* ARGB8888 for the chrome's shadow and for a translucent window. */
static uint32_t buffer_format(const struct win *wi)
{
    return wi->csd.enabled || wi->translucent ? 2 : 1;
}

static void pool_destroy(struct pool *p)
{
    for (int i = 0; i < GUI_SLOTS; i++)
        if (p->proxy[i])
            buffer_destroy(p->proxy[i]);
    if (p->pool)
        shm_pool_destroy(p->pool);
    if (p->map)
        munmap(p->map, p->size);
    if (p->fd >= 0)
        close(p->fd);
    memset(p, 0, sizeof *p);
    p->fd = -1;
}

/* Destroy the old pools whose buffers the compositor has released. */
static void reap_old_pools(struct win *wi)
{
    for (int i = 0; i < wi->nold;) {
        struct pool *p = &wi->old[i];
        int busy = 0;
        for (int k = 0; k < GUI_SLOTS; k++)
            busy |= p->proxy[k] && p->busy[k];
        if (busy) {
            i++;
            continue;
        }
        pool_destroy(p);
        wi->old[i] = wi->old[--wi->nold];
    }
}

/* Replace the pool with one whose slots hold need bytes and a quarter
 * more, so that a window that grows a little reuses it. The previous pool
 * remains until the compositor releases its buffers. */
static int new_pool(struct win *wi, size_t need)
{
    size_t cap = (need + need / 4 + 65535) & ~(size_t)65535, size = cap * GUI_SLOTS;
    int fd = memfd_create("gui", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (long)size) < 0) {
        fprintf(stderr, "gui: cannot allocate a buffer pool of %zu bytes: %s\n", size, strerror(errno));
        if (fd >= 0)
            close(fd);
        return -1;
    }
    uint8_t *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        fprintf(stderr, "gui: cannot map a buffer pool of %zu bytes: %s\n", size, strerror(errno));
        close(fd);
        return -1;
    }
    if (wi->pool.pool) {
        struct pool *old = realloc(wi->old, (size_t)(wi->nold + 1) * sizeof *old);
        if (!old) {
            munmap(map, size);
            close(fd);
            return -1;
        }
        wi->old = old;
        struct pool *p = &wi->old[wi->nold++];
        *p = wi->pool;
        for (int i = 0; i < GUI_SLOTS; i++)
            p->busy[i] = wi->bufs.slot[i].busy;
    }
    memset(&wi->pool, 0, sizeof wi->pool);
    wi->pool.pool = shm_create_pool(shm, fd, (int32_t)size);
    wi->pool.fd = fd;
    wi->pool.map = map;
    wi->pool.size = size;
    wi->pool.cap = cap;
    for (int i = 0; i < GUI_SLOTS; i++) {
        wi->bufs.slot[i] = (struct gui_slot){ (uint32_t *)(map + (size_t)i * cap), 0, 0, 0, { 0 } };
    }
    wi->bufs.nslots = GUI_SLOTS;
    return 0;
}

/* Give slot i a buffer of the window geometry and format. A slot whose
 * geometry changes differs everywhere from the current slot. Returns -1
 * when the compositor still reads the slot's old buffer. */
static int ensure_buffer(struct win *wi, int i)
{
    struct gui_buffers *b = &wi->bufs;
    struct gui_slot *s = &b->slot[i];
    uint32_t format = buffer_format(wi);
    if (wi->pool.proxy[i] && s->w == b->w && s->h == b->h && wi->pool.format[i] == format)
        return 0;
    if (s->busy)
        return -1;
    if (wi->pool.proxy[i])
        buffer_destroy(wi->pool.proxy[i]);
    extern const struct buffer_listener buffer_events;
    struct wire_proxy *proxy = shm_pool_create_buffer(wi->pool.pool, (int32_t)((size_t)i * wi->pool.cap), b->w, b->h,
                                                      b->w * 4, format);
    buffer_add_listener(proxy, &buffer_events, wi->w);
    wi->pool.proxy[i] = proxy;
    wi->pool.format[i] = format;
    wi->pool.used[i] = 1;
    if (s->w != b->w || s->h != b->h) {
        s->w = b->w;
        s->h = b->h;
        rect_set_clear(&s->stale);
        rect_set_add(&s->stale, (struct rect){ 0, 0, b->w, b->h });
    }
    return 0;
}

static void on_release(void *user, struct wire_proxy *proxy)
{
    struct gui_window *w = user;
    struct win *wi = w->priv;
    for (int i = 0; i < GUI_SLOTS; i++)
        if (wi->pool.proxy[i] == proxy)
            wi->bufs.slot[i].busy = 0;
    for (int k = 0; k < wi->nold; k++)
        for (int i = 0; i < GUI_SLOTS; i++)
            if (wi->old[k].proxy[i] == proxy)
                wi->old[k].busy[i] = 0;
    reap_old_pools(wi);
}
const struct buffer_listener buffer_events = { on_release };

/* Point gui_window.surf at the contents in the current slot. */
static void point_surface(struct gui_window *w)
{
    struct win *wi = w->priv;
    w->surf.pixels = wi->bufs.slot[wi->bufs.cur].pixels + wi->content_off;
    w->surf.stride = wi->bufs.w;
}

/* Paint the chrome that chrome_repaint marked into the current slot. */
static void paint_chrome(struct gui_window *w)
{
    struct win *wi = w->priv;
    if (!wi->chrome_dirty || !wi->bufs.nslots)
        return;
    struct surface full = gui_buffers_surface(&wi->bufs);
    int s = w->scale > 0 ? w->scale : 1;
    struct rect r = wi->chrome_dirty & 2 ? csd_paint(&full, s, &wi->csd, w->width, w->height)
                                         : csd_paint_header(&full, s, &wi->csd, w->width, w->height);
    add_damage(wi, r);
    wi->chrome_dirty = 0;
}

/* Wait up to ms milliseconds for events and dispatch them. */
static int wait_display(long ms)
{
    wire_display_flush(display);
    struct pollfd pf = { wire_display_fd(display), POLLIN, 0 };
    int r = poll(&pf, 1, ms < 0 ? 0 : (int)ms);
    if (r < 0 && errno != EINTR)
        return -1;
    if (r > 0 && dispatch_events() < 0)
        return -1;
    return 0;
}

void gui_begin_paint(struct gui_window *w)
{
    struct win *wi = w->priv;
    struct gui_buffers *b = &wi->bufs;
    long deadline = -1;
    while (display && b->nslots && b->committed) {
        int t = gui_buffers_pick(b, 2);
        /* The two slots remain busy for 100 ms: a third one helps. */
        if (t < 0 && deadline >= 0 && uptime_ms() >= deadline)
            t = gui_buffers_pick(b, GUI_SLOTS);
        if (t >= 0 && ensure_buffer(wi, t) == 0) {
            long t0 = uptime_us();
            stats.copied_bytes += gui_buffers_switch(b, t);
            stats.copy_us += (uint64_t)(uptime_us() - t0);
            point_surface(w);
            break;
        }
        if (deadline < 0) {
            deadline = uptime_ms() + 100;
            stats.frame_waits++;
        }
        if (wait_display(deadline - uptime_ms() > 0 ? deadline - uptime_ms() : 1) < 0)
            break;
    }
    paint_chrome(w);
}

int gui_frame_pending(const struct gui_window *w)
{
    return ((const struct win *)w->priv)->frame_pending;
}

/* Size the surface and the buffers for contents of width by height
 * logical pixels at the output's scale, with the chrome around them.
 * Damage is recorded in device pixels of the whole surface. */
static void surface_resize(struct gui_window *w, int width, int height)
{
    struct win *wi = w->priv;
    struct gui_buffers *b = &wi->bufs;
    int scale = output_scale();
    int bw, bh;
    csd_buffer_size(&wi->csd, width, height, &bw, &bh);
    int dw = bw * scale, dh = bh * scale;
    /* The newest contents, copied into the new geometry. They remain
     * mapped until reap_old_pools, which runs last. */
    struct surface old = w->surf;
    int old_w = b->w, old_h = b->h;
    int old_committed = b->committed;
    /* The old contents' origin in the old buffer. The CSD state may have
     * changed since, so the origin comes from the offset. */
    int ox = old.stride ? (int)(wi->content_off % (size_t)old.stride) : 0;
    int oy = old.stride ? (int)(wi->content_off / (size_t)old.stride) : 0;
    size_t need = (size_t)dw * dh * 4;
    b->w = dw;
    b->h = dh;
    int t = wi->pool.pool && need <= wi->pool.cap ? gui_buffers_pick(b, GUI_SLOTS) : -1;
    if (t < 0 && new_pool(wi, need) == 0)
        t = 0;
    if (t < 0 || ensure_buffer(wi, t) < 0) {
        /* Without buffers of the new size the window commits nothing: a
         * buffer of the old size would contradict the acknowledged
         * configure. The next successful resize ends this state. */
        b->w = old_w;
        b->h = old_h;
        wi->resize_failed = 1;
        return;
    }
    wi->resize_failed = 0;
    b->cur = t;
    b->committed = 0;
    struct rect c = csd_content(&wi->csd, width, height);
    wi->content_off = (size_t)c.y * scale * dw + (size_t)c.x * scale;
    w->surf = (struct surface){ b->slot[t].pixels + wi->content_off, width * scale, height * scale, dw };
    gfx_fill(&w->surf, 0x00dcdcdc);
    if (old.pixels) {
        gfx_blit(&w->surf, 0, 0, &old, NULL);
        /* A commit blended the old bottom corners. The store has their
         * raw pixels. */
        if (old_committed)
            gui_buffers_put_raw_corners(b, &w->surf, -ox, -oy);
    }
    w->width = width;
    w->height = height;
    w->scale = scale;
    gui_buffers_stale_all(b);
    struct rect corners[4];
    gui_buffers_set_corners(b, corners, csd_corner_rects(&wi->csd, width, height, scale, corners));
    surface_set_buffer_scale(wi->surface, scale);
    struct rect rects[5];
    int n = wi->translucent ? 0 : csd_opaque_region(&wi->csd, width, height, rects);
    struct wire_array region = { rects, sizeof rects[0] * (size_t)n };
    surface_set_opaque_region(wi->surface, &region);
    if (wi->csd.enabled) {
        n = csd_input_region(&wi->csd, width, height, rects);
        struct wire_array input = { rects, sizeof rects[0] * (size_t)n };
        surface_set_input_region(wi->surface, &input);
        struct rect f = csd_frame(&wi->csd, width, height);
        if (wi->toplevel)
            toplevel_set_window_geometry(wi->toplevel, f.x, f.y, f.w, f.h);
        struct surface full = gui_buffers_surface(b);
        csd_paint(&full, scale, &wi->csd, width, height);
    }
    wi->chrome_dirty = 0;
    rect_set_clear(&wi->damage);
    rect_set_add(&wi->damage, (struct rect){ 0, 0, dw, dh });
    reap_old_pools(wi);
}

static void commit_now(struct gui_window *w);

static void on_frame_done(void *user, struct wire_proxy *cb, uint32_t t)
{
    struct gui_window *w = user;
    struct win *wi = w->priv;
    wire_proxy_destroy(cb);
    wi->frame_cb = NULL;
    wi->frame_pending = 0;
}
static const struct callback_listener frame_events = { on_frame_done };

/* Commit the current slot with the damage since the last commit. A
 * pending frame defers the commit to a later gui_flush, which follows the
 * frame callback. */
static void commit_now(struct gui_window *w)
{
    struct win *wi = w->priv;
    struct gui_buffers *b = &wi->bufs;
    if (wi->chrome_dirty && !wi->frame_pending)
        gui_begin_paint(w);
    if (!wi->damage.n || !b->nslots || wi->resize_failed)
        return;
    if (wi->frame_pending) {
        if (!wi->need_commit)
            stats.frame_waits++;
        wi->need_commit = 1;
        return;
    }
    if (ensure_buffer(wi, b->cur) < 0)
        return;
    int s = w->scale > 0 ? w->scale : 1;
    /* A slot committed again without gui_begin_paint has blended corners
     * already. */
    if (!b->committed) {
        long t0 = uptime_us();
        gui_buffers_commit(b, &wi->damage);
        struct surface full = gui_buffers_surface(b);
        csd_finish_corners(&full, s, &wi->csd, w->width, w->height);
        stats.copy_us += (uint64_t)(uptime_us() - t0);
    }
    surface_attach(wi->surface, wi->pool.proxy[b->cur], 0, 0);
    /* The protocol takes surface (logical) coordinates: round outwards. */
    for (int i = 0; i < wi->damage.n; i++) {
        struct rect r = wi->damage.r[i];
        int lx0 = r.x / s, ly0 = r.y / s, lx1 = (r.x + r.w + s - 1) / s, ly1 = (r.y + r.h + s - 1) / s;
        surface_damage(wi->surface, lx0, ly0, lx1 - lx0, ly1 - ly0);
    }
    struct wire_proxy *cb = surface_frame(wi->surface);
    callback_add_listener(cb, &frame_events, w);
    wi->frame_cb = cb;
    surface_commit(wi->surface);
    stats.commits++;
    wi->frame_pending = 1;
    wi->need_commit = 0;
    rect_set_clear(&wi->damage);
    reap_old_pools(wi);
}

void gui_get_stats(struct gui_stats *out)
{
    *out = stats;
    out->pool_bytes = 0;
    for (struct gui_window *w = wins; w; w = w->next) {
        struct win *wi = w->priv;
        for (int i = 0; i < GUI_SLOTS; i++)
            out->pool_bytes += wi->pool.used[i] ? wi->pool.cap : 0;
        for (int k = 0; k < wi->nold; k++)
            for (int i = 0; i < GUI_SLOTS; i++)
                out->pool_bytes += wi->old[k].used[i] ? wi->old[k].cap : 0;
    }
}

void gui_count_paint(long us)
{
    stats.paints++;
    stats.paint_us += (uint64_t)us;
}

void gui_count(enum gui_counter counter, uint64_t n)
{
    switch (counter) {
    case GUI_COUNT_WIDGET_PAINTS: stats.widget_paints += n; break;
    case GUI_COUNT_PAINTED_PIXELS: stats.painted_pixels += n; break;
    case GUI_COUNT_LAYOUTS: stats.layouts += n; break;
    case GUI_COUNT_TEXT_SHAPES: stats.text_shapes += n; break;
    }
}

void gui_flush(void)
{
    for (struct gui_window *w = wins; w; w = w->next)
        commit_now(w);
    if (display)
        wire_display_flush(display);
}

/* ---- windows ---- */

static void on_configure(void *user, struct wire_proxy *t, uint32_t serial, int32_t width, int32_t height, const struct wire_array *states)
{
    struct gui_window *w = window_of_toplevel(t);
    if (!w)
        return;
    struct win *wi = w->priv;
    toplevel_ack_configure(t, serial);
    int maximized = 0, active = 0;
    const uint32_t *st = states->data;
    for (size_t i = 0; i < states->size / 4; i++) {
        if (st[i] == 1) maximized = 1;
        if (st[i] == 2) active = 1;
    }
    int chrome_changed = wi->csd.enabled && (maximized != wi->csd.maximized || active != wi->csd.active);
    int layout_changed = wi->csd.enabled && maximized != wi->csd.maximized;
    wi->csd.maximized = maximized;
    wi->csd.active = active;
    /* The size describes the window geometry: the header bar is part of it. */
    if (width > 0 && height > 0) {
        height -= csd_header(&wi->csd);
        if (wi->min_w && width < wi->min_w) width = wi->min_w;
        if (wi->min_h && height < wi->min_h) height = wi->min_h;
        if (height < 1) height = 1;
    }
    if (width > 0 && height > 0 && (width != w->width || height != w->height || layout_changed)) {
        int changed = width != w->width || height != w->height;
        surface_resize(w, width, height);
        if (changed) {
            struct wmsg m = { WM_RESIZED, 0, w->id, width, height, 0, 0, "" };
            push(&m);
        }
    } else if (layout_changed && w->surf.pixels) {
        surface_resize(w, w->width, w->height);
    } else if (chrome_changed) {
        chrome_repaint(w, 0);
    }
}
static void on_decor_mode(void *user, struct wire_proxy *d, uint32_t mode)
{
    struct gui_window *w = user;
    struct win *wi = w->priv;
    wi->csd.enabled = mode == 2;
}
static const struct decoration_listener decor_events = { on_decor_mode };

/* Ask for client side decorations; the compositor's answer arrives with
 * the roundtrip that follows. */
static void decorate(struct gui_window *w, const char *title)
{
    struct win *wi = w->priv;
    strlcpy(wi->csd.title, title ? title : "", sizeof wi->csd.title);
    wi->decoration = shell_get_decoration(shell, wi->toplevel);
    decoration_add_listener(wi->decoration, &decor_events, w);
    decoration_set_mode(wi->decoration, 2);
}
static void on_close(void *user, struct wire_proxy *t)
{
    struct gui_window *w = window_of_toplevel(t);
    if (w) {
        struct wmsg m = { WM_CLOSE, 0, w->id, 0, 0, 0, 0, "" };
        push(&m);
    }
}
static const struct toplevel_listener toplevel_events = { on_configure, on_close };
static void on_enter_output(void *user, struct wire_proxy *s, struct wire_proxy *o) {}
static const struct surface_listener surface_events = { on_enter_output };

static void on_layer_configure(void *user, struct wire_proxy *l, uint32_t serial, int32_t width, int32_t height)
{
    struct gui_window *w = NULL;
    for (struct gui_window *k = wins; k; k = k->next)
        if (((struct win *)k->priv)->layer == l)
            w = k;
    if (!w)
        return;
    layer_surface_ack_configure(l, serial);
    if (width > 0 && height > 0 && (width != w->width || height != w->height)) {
        surface_resize(w, width, height);
        struct wmsg m = { WM_RESIZED, 0, w->id, width, height, 0, 0, "" };
        push(&m);
    }
}
static void on_layer_closed(void *user, struct wire_proxy *l)
{
    for (struct gui_window *k = wins; k; k = k->next)
        if (((struct win *)k->priv)->layer == l) {
            struct wmsg m = { WM_CLOSE, 0, k->id, 0, 0, 0, 0, "" };
            push(&m);
        }
}
static const struct layer_surface_listener layer_events = { on_layer_configure, on_layer_closed };

static void on_lock_configure(void *user, struct wire_proxy *l, uint32_t serial, int32_t width, int32_t height)
{
    struct gui_window *w = user;
    lock_surface_ack_configure(l, serial);
    if (width > 0 && height > 0 && (width != w->width || height != w->height)) {
        surface_resize(w, width, height);
        struct wmsg m = { WM_RESIZED, 0, w->id, width, height, 0, 0, "" };
        push(&m);
    }
}
static const struct lock_surface_listener lock_surface_events = { on_lock_configure };

static void on_locked(void *user, struct wire_proxy *lock)
{
    struct gui_window *w = user;
    ((struct win *)w->priv)->lock_state = 1;
}

/* X12 refused the lock, or the session ended while it was locked. A lock
 * window that was shown closes. */
static void on_lock_finished(void *user, struct wire_proxy *lock)
{
    struct gui_window *w = user;
    struct win *wi = w->priv;
    if (wi->lock_state == 1) {
        struct wmsg m = { WM_CLOSE, 0, w->id, 0, 0, 0, 0, "" };
        push(&m);
    }
    wi->lock_state = -1;
}
static const struct session_lock_listener session_lock_events = { on_locked, on_lock_finished };

static struct gui_window *window_of_popup(struct wire_proxy *p)
{
    for (struct gui_window *w = wins; w; w = w->next)
        if (((struct win *)w->priv)->popup == p)
            return w;
    return NULL;
}

static void on_popup_configure(void *user, struct wire_proxy *p, uint32_t serial, int32_t x, int32_t y, int32_t width, int32_t height)
{
    struct gui_window *w = window_of_popup(p);
    if (!w)
        return;
    popup_ack_configure(p, serial);
    if (width > 0 && height > 0 && (width != w->width || height != w->height))
        surface_resize(w, width, height);
}

static void on_popup_done(void *user, struct wire_proxy *p)
{
    struct gui_window *w = window_of_popup(p);
    if (w) {
        struct wmsg m = { WM_CLOSE, 0, w->id, 0, 0, 0, 0, "" };
        push(&m);
    }
}
static const struct popup_listener popup_events = { on_popup_configure, on_popup_done };

static struct gui_window *window_alloc(void)
{
    struct gui_window *w = calloc(1, sizeof *w);
    struct win *wi = calloc(1, sizeof *wi);
    if (!w || !wi) {
        free(w);
        free(wi);
        return NULL;
    }
    wi->pool.fd = -1;
    wi->w = w;
    w->priv = wi;
    w->id = next_id++;
    wi->surface = compositor_create_surface(compositor);
    surface_add_listener(wi->surface, &surface_events, NULL);
    w->next = wins;
    wins = w;
    return w;
}

static void roundtrip(void);

struct gui_window *gui_create_layer_window(int width, int height, int layer, int anchor, int exclusive,
                                           int keyboard, const char *ns)
{
    if (!display)
        return NULL;
    struct gui_window *w = window_alloc();
    if (!w)
        return NULL;
    struct win *wi = w->priv;
    wi->layer = shell_get_layer_surface(shell, wi->surface, (uint32_t)layer, ns);
    layer_surface_add_listener(wi->layer, &layer_events, NULL);
    layer_surface_set_anchor(wi->layer, (uint32_t)anchor);
    layer_surface_set_exclusive_zone(wi->layer, exclusive);
    layer_surface_set_keyboard_interactivity(wi->layer, (uint32_t)keyboard);
    layer_surface_set_size(wi->layer, width, height);
    /* The compositor answers with the size (the free desktop area when
     * a dimension is 0); wait for it so the caller sees the real size. */
    roundtrip();
    if (w->width == 0)
        surface_resize(w, width > 0 ? width : screen_w, height > 0 ? height : screen_h);
    /* Nothing is committed yet: the surface maps with the caller's first
     * drawing, not with the grey fill of the new buffer, which would
     * flash over the whole screen for a full screen overlay. */
    return w;
}

struct gui_window *gui_create_lock_window(void)
{
    if (!display)
        return NULL;
    struct wire_proxy *manager = gui_bind_global("session_lock_manager", &session_lock_manager_interface, 1);
    if (!manager)
        return NULL;
    struct gui_window *w = window_alloc();
    if (!w) {
        wire_proxy_destroy(manager);
        return NULL;
    }
    struct win *wi = w->priv;
    wi->session_lock = session_lock_manager_lock(manager);
    session_lock_add_listener(wi->session_lock, &session_lock_events, w);
    wi->lock_surface = session_lock_get_lock_surface(wi->session_lock, wi->surface);
    lock_surface_add_listener(wi->lock_surface, &lock_surface_events, w);
    wire_proxy_destroy(manager);
    wire_display_flush(display);
    /* X12 sends locked after it has composed a frame without the session,
     * or finished at once. The wait ends after 5 seconds. */
    for (int i = 0; i < 500 && wi->lock_state == 0; i++) {
        struct pollfd pf = { wire_display_fd(display), POLLIN, 0 };
        poll(&pf, 1, 10);
        if (dispatch_events() < 0)
            break;
    }
    if (wi->lock_state != 1) {
        gui_destroy_window(w);
        return NULL;
    }
    if (w->width == 0)
        surface_resize(w, screen_w, screen_h);
    return w;
}

int gui_unlock_session(struct gui_window *w)
{
    struct win *wi = w ? w->priv : NULL;
    if (!wi || !wi->session_lock || wi->lock_state != 1)
        return -1;
    session_lock_unlock_and_destroy(wi->session_lock);
    wi->session_lock = NULL;
    wi->lock_state = 0;
    wire_display_flush(display);
    return 0;
}

void gui_layer_set_margin(struct gui_window *w, int top, int right, int bottom, int left)
{
    struct win *wi = w ? w->priv : NULL;
    if (wi && wi->layer)
        layer_surface_set_margin(wi->layer, top, right, bottom, left);
}

struct gui_window *gui_create_window(int width, int height, const char *title)
{
    if (!display)
        return NULL;
    struct gui_window *w = window_alloc();
    if (!w)
        return NULL;
    struct win *wi = w->priv;
    wi->toplevel = shell_get_toplevel(shell, wi->surface);
    toplevel_add_listener(wi->toplevel, &toplevel_events, NULL);
    toplevel_set_title(wi->toplevel, title);
    /* The panel shows the icon of the program (launcher_icon_name). */
    if (getprogname()[0])
        toplevel_set_app_id(wi->toplevel, getprogname());
    decorate(w, title);
    /* A role must receive and acknowledge its initial configure before
     * the first non-NULL buffer is committed. */
    roundtrip();
    surface_resize(w, width, height);
    gui_flush();
    return w;
}

struct gui_window *gui_create_dialog_window(struct gui_window *parent, int width, int height, const char *title)
{
    if (!display)
        return NULL;
    struct gui_window *w = window_alloc();
    if (!w)
        return NULL;
    struct win *wi = w->priv, *pi = parent ? parent->priv : NULL;
    wi->toplevel = shell_get_toplevel(shell, wi->surface);
    toplevel_add_listener(wi->toplevel, &toplevel_events, NULL);
    toplevel_set_title(wi->toplevel, title);
    /* The panel shows the icon of the program (launcher_icon_name). */
    if (getprogname()[0])
        toplevel_set_app_id(wi->toplevel, getprogname());
    if (pi && pi->toplevel) {
        toplevel_set_parent(wi->toplevel, pi->toplevel);
        toplevel_set_modal(wi->toplevel, 1);
    }
    decorate(w, title);
    roundtrip();
    surface_resize(w, width, height);
    gui_flush();
    return w;
}

int gui_has_popup_surfaces(void) { return display != NULL; }

void gui_text_input_set(struct gui_window *window, int enabled)
{
    if (!text_input || !window)
        return;
    struct win *wi = window->priv;
    if (enabled) {
        text_input_enable(text_input, wi->surface);
        memset(text_cursor, 0, sizeof text_cursor);
        text_input_set_surrounding_text(text_input, "", 0, 0);
        text_input_set_content_type(text_input, 0, 0);
    } else {
        text_input_disable(text_input);
        text_active = 0;
        text_win = NULL;
    }
    text_input_commit(text_input, ++text_serial);
}

/* Only a changed rectangle is sent.  A new enable sends it again. */
void gui_text_input_set_cursor(struct gui_window *window, int x, int y, int width, int height)
{
    if (!text_input || !window)
        return;
    int ox, oy;
    content_origin(window, &ox, &oy);
    int r[4] = { x + ox, y + oy, width, height };
    if (memcmp(r, text_cursor, sizeof r) == 0)
        return;
    memcpy(text_cursor, r, sizeof r);
    text_input_set_cursor_rectangle(text_input, r[0], r[1], r[2], r[3]);
    text_input_commit(text_input, ++text_serial);
}

struct gui_window *gui_create_popup_window(struct gui_window *parent, int x, int y, int width, int height, int grab)
{
    if (!display || !parent)
        return NULL;
    struct gui_window *w = window_alloc();
    if (!w)
        return NULL;
    struct win *wi = w->priv, *pi = parent->priv;
    int ox, oy;
    content_origin(parent, &ox, &oy);
    struct wire_proxy *pos = shell_create_positioner(shell);
    positioner_set_size(pos, width, height);
    positioner_set_anchor_rect(pos, x + ox, y + oy, 1, 1);
    positioner_set_anchor(pos, 5);             /* top-left */
    positioner_set_gravity(pos, 8);            /* extend down and right */
    positioner_set_constraint_adjustment(pos, 63); /* flip, slide, then resize on both axes */
    wi->popup = shell_get_popup(shell, wi->surface, pi->surface, pos);
    popup_add_listener(wi->popup, &popup_events, NULL);
    positioner_destroy(pos);
    roundtrip();
    if (w->width == 0)
        surface_resize(w, width, height);
    if (grab)
        popup_grab(wi->popup, seat, last_serial);
    gui_flush();
    return w;
}

void gui_destroy_window(struct gui_window *w)
{
    struct win *wi = w->priv;
    for (struct gui_window **pp = &wins; *pp; pp = &(*pp)->next)
        if (*pp == w) {
            *pp = w->next;
            break;
        }
    if (pointer_win == w) pointer_win = NULL;
    if (keyboard_win == w) keyboard_win = NULL;
    /* The event of a pending frame callback would reach the freed window,
     * so the callback is destroyed with it, and libwire drops the event. */
    if (wi->frame_cb)
        wire_proxy_destroy(wi->frame_cb);
    if (wi->decoration)
        decoration_destroy(wi->decoration);
    if (wi->toplevel)
        toplevel_destroy(wi->toplevel);
    if (wi->layer)
        layer_surface_destroy(wi->layer);
    if (wi->lock_surface)
        lock_surface_destroy(wi->lock_surface);
    /* A lock that was not unlocked remains in effect without a locker. */
    if (wi->session_lock)
        session_lock_destroy(wi->session_lock);
    if (wi->popup)
        popup_destroy(wi->popup);
    if (wi->surface)
        surface_destroy(wi->surface);
    pool_destroy(&wi->pool);
    for (int k = 0; k < wi->nold; k++)
        pool_destroy(&wi->old[k]);
    free(wi->old);
    gui_buffers_free(&wi->bufs);
    wire_display_flush(display);
    free(wi);
    free(w);
}

void gui_damage(struct gui_window *w, int x, int y, int width, int height)
{
    struct win *wi = w->priv;
    int s = w->scale > 0 ? w->scale : 1;
    struct rect r = rect_intersect((struct rect){ x * s, y * s, width * s, height * s },
                                   (struct rect){ 0, 0, w->surf.width, w->surf.height });
    if (rect_empty(r))
        return;
    int ox, oy;
    content_origin(w, &ox, &oy);
    r.x += ox * s;
    r.y += oy * s;
    add_damage(wi, r);
}

/* Regions given in contents coordinates, moved into the surface. */
static void offset_region(struct gui_window *w, const struct rect *rects, int count, struct rect *out)
{
    int ox, oy;
    content_origin(w, &ox, &oy);
    for (int i = 0; i < count; i++)
        out[i] = (struct rect){ rects[i].x + ox, rects[i].y + oy, rects[i].w, rects[i].h };
}

void gui_set_translucent(struct gui_window *w)
{
    struct win *wi = w->priv;
    if (wi->translucent)
        return;
    wi->translucent = 1;
    /* The next buffers have the ARGB format. A slot on screen is left
     * through gui_begin_paint. */
    if (wi->bufs.committed)
        gui_begin_paint(w);
    struct rect none[1];
    struct wire_array region = { none, 0 };
    surface_set_opaque_region(wi->surface, &region);
    add_damage(wi, (struct rect){ 0, 0, wi->bufs.w, wi->bufs.h });
}

void gui_set_opaque_region(struct gui_window *w, const struct rect *rects, int count)
{
    if (!w || count < 0 || count > 16)
        return;
    struct rect moved[16];
    offset_region(w, rects, count, moved);
    struct wire_array a = { moved, (size_t)count * sizeof *rects };
    surface_set_opaque_region(((struct win *)w->priv)->surface, &a);
}

void gui_set_input_region(struct gui_window *w, const struct rect *rects, int count)
{
    if (!w || count < 0 || count > 16)
        return;
    struct rect moved[16];
    offset_region(w, rects, count, moved);
    struct wire_array a = { moved, (size_t)count * sizeof *rects };
    surface_set_input_region(((struct win *)w->priv)->surface, &a);
}

void gui_move(struct gui_window *w, int x, int y) {}

void gui_set_title(struct gui_window *w, const char *title)
{
    struct win *wi = w->priv;
    if (wi->toplevel)
        toplevel_set_title(wi->toplevel, title);
    strlcpy(wi->csd.title, title ? title : "", sizeof wi->csd.title);
    chrome_repaint(w, 1);
}

void gui_resize(struct gui_window *w, int width, int height)
{
    if (width == w->width && height == w->height)
        return;
    surface_resize(w, width, height);
    struct wmsg m = { WM_RESIZED, 0, w->id, width, height, 0, 0, "" };
    push(&m);
}

void gui_set_min_size(struct gui_window *w, int width, int height)
{
    struct win *wi = w->priv;
    wi->min_w = width;
    wi->min_h = height;
    if (wi->toplevel)
        toplevel_set_min_size(wi->toplevel, width, height + csd_header(&wi->csd));
}

/* ---- clipboard ---- */

static int sync_done;
static void on_sync(void *user, struct wire_proxy *cb, uint32_t t) { sync_done = 1; wire_proxy_destroy(cb); }
static const struct callback_listener sync_events = { on_sync };

/* Wait (up to a second) until the compositor has answered everything sent so far. */
static void roundtrip(void)
{
    struct wire_proxy *cb = display_sync(wire_display_proxy(display));
    callback_add_listener(cb, &sync_events, NULL);
    sync_done = 0;
    wire_display_flush(display);
    for (int i = 0; i < 100 && !sync_done; i++) {
        struct pollfd pf = { wire_display_fd(display), POLLIN, 0 };
        poll(&pf, 1, 10);
        if (dispatch_events() < 0)
            return;
    }
}

int gui_clipboard_set(const char *text, int len)
{
    if (!data_device)
        return -1;
    if (len > WSRV_CLIP_MAX)
        len = WSRV_CLIP_MAX;
    free(clip_text);
    clip_text = malloc((size_t)len + 1);
    if (!clip_text)
        return -1;
    memcpy(clip_text, text, (size_t)len);
    clip_text[len] = '\0';
    clip_len = len;
    if (selection_source)
        data_source_destroy(selection_source);
    selection_source = data_device_manager_create_data_source(data_manager);
    data_source_add_listener(selection_source, &source_events, NULL);
    data_source_offer(selection_source, "text/plain");
    data_device_set_selection(data_device, selection_source, last_serial);
    /* The compositor fetches a copy right away; serve it before returning. */
    roundtrip();
    roundtrip();
    return 0;
}

int gui_clipboard_get(char *buf, int size)
{
    if (!data_device || size <= 0)
        return -1;
    roundtrip();
    /* Our own selection: the compositor would route the transfer back
     * to this process, which is busy here; answer directly. */
    if (selection_source || !selection_offer) {
        if (!clip_text)
            return -1;
        int n = clip_len < size ? clip_len : size;
        memcpy(buf, clip_text, (size_t)n);
        if (n < size)
            buf[n] = '\0';
        return n;
    }
    int p[2];
    if (pipe2(p, O_CLOEXEC) < 0)
        return -1;
    data_offer_receive(selection_offer, "text/plain", p[1]);
    close(p[1]);
    wire_display_flush(display);
    int n = 0;
    fcntl(p[0], F_SETFL, O_NONBLOCK);
    for (int i = 0; i < 200 && n < size; i++) {
        struct pollfd pf = { p[0], POLLIN, 0 };
        if (poll(&pf, 1, 10) <= 0)
            continue;
        ssize_t k = read(p[0], buf + n, (size_t)(size - n));
        if (k == 0 || (k < 0 && errno != EAGAIN))
            break;
        if (k > 0)
            n += (int)k;
    }
    close(p[0]);
    if (n < size)
        buf[n] = '\0';
    return n;
}

/* ---- drag and drop ---- */

/* The drag icon: a surface without a role whose single buffer contains a
 * copy of the image. */
static struct wire_proxy *icon_create(struct gui_window *w, const struct surface *icon)
{
    size_t size = (size_t)icon->width * icon->height * 4;
    int fd = memfd_create("gui-drag", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (long)size) < 0) {
        if (fd >= 0)
            close(fd);
        return NULL;
    }
    uint32_t *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    for (int y = 0; y < icon->height; y++)
        memcpy(map + (size_t)y * icon->width, icon->pixels + (size_t)y * icon->stride, (size_t)icon->width * 4);
    drag.icon_fd = fd;
    drag.icon_map = map;
    drag.icon_size = size;
    drag.icon_pool = shm_create_pool(shm, fd, (int32_t)size);
    drag.icon_buf = shm_pool_create_buffer(drag.icon_pool, 0, icon->width, icon->height, icon->width * 4, 2);
    drag.icon = compositor_create_surface(compositor);
    surface_set_buffer_scale(drag.icon, w->scale > 0 ? w->scale : 1);
    return drag.icon;
}

int gui_drag_start(struct gui_window *w, const struct gui_drag_item *items, int nitems, int actions,
                   const struct surface *icon, int hot_x, int hot_y)
{
    if (!data_device)
        return -ENOTSUP;
    if (!w || !items || nitems <= 0 || nitems > 8 || !(actions & (GUI_DND_COPY | GUI_DND_MOVE)))
        return -EINVAL;
    drag_end(0);
    for (int i = 0; i < nitems; i++) {
        char *mime = strdup(items[i].mime);
        char *data = malloc(items[i].len + 1);
        if (!mime || !data) {
            free(mime);
            free(data);
            for (int k = 0; k < drag.nitems; k++) {
                free((void *)drag.items[k].mime);
                free((void *)drag.items[k].data);
            }
            drag.nitems = 0;
            return -ENOMEM;
        }
        memcpy(data, items[i].data, items[i].len);
        data[items[i].len] = '\0';
        drag.items[i] = (struct gui_drag_item){ mime, data, items[i].len };
        drag.nitems = i + 1;
    }
    struct win *wi = w->priv;
    drag.window = w->id;
    drag_source = data_device_manager_create_data_source(data_manager);
    data_source_add_listener(drag_source, &drag_source_events, NULL);
    for (int i = 0; i < nitems; i++)
        data_source_offer(drag_source, drag.items[i].mime);
    data_source_set_actions(drag_source, (uint32_t)(actions & (GUI_DND_COPY | GUI_DND_MOVE)));
    struct wire_proxy *ic = icon && icon->pixels && icon->width > 0 && icon->height > 0 ? icon_create(w, icon) : NULL;
    data_device_start_drag(data_device, drag_source, wi->surface, ic, button_serial);
    if (ic) {
        int s = w->scale > 0 ? w->scale : 1;
        surface_attach(ic, drag.icon_buf, -hot_x, -hot_y);
        surface_damage(ic, 0, 0, icon->width / s, icon->height / s);
        surface_commit(ic);
    }
    wi->buttons = 0;            /* the release of the button goes to the drag */
    wire_display_flush(display);
    return 0;
}

int gui_dragging(void) { return drag_source != NULL; }

int gui_drag_offers(const char *mime)
{
    struct offer_info *i = offer_info(target_offer);
    for (int k = 0; i && mime && k < i->nmimes; k++)
        if (strcmp(i->mimes[k], mime) == 0)
            return 1;
    return 0;
}

void gui_drag_accept(const char *mime, int actions, int preferred)
{
    if (!target_offer)
        return;
    if (mime && !*mime)
        mime = NULL;
    if (strcmp(target_mime, mime ? mime : "") != 0) {
        data_offer_accept(target_offer, target_serial, mime);
        strlcpy(target_mime, mime ? mime : "", sizeof target_mime);
    }
    if (mime && (actions != target_actions || preferred != target_preferred)) {
        data_offer_set_actions(target_offer, (uint32_t)actions, (uint32_t)preferred);
        target_actions = actions;
        target_preferred = preferred;
    }
    wire_display_flush(display);
}

const char *gui_drop_data(size_t *len, const char **mime)
{
    if (len)
        *len = drop_len;
    if (mime)
        *mime = drop_mime;
    return drop_buf;
}

const char *gui_drag_peek(const char *mime, size_t *len)
{
    struct offer_info *i = offer_info(target_offer);
    if (!i || !mime || !gui_drag_offers(mime))
        return NULL;
    if (drag_source) {                  /* a drag of this process */
        const struct gui_drag_item *it = drag_item(mime);
        if (len)
            *len = it ? it->len : 0;
        return it ? it->data : NULL;
    }
    if (strcmp(i->peek_mime, mime) == 0) {
        if (i->peek && len)
            *len = i->peek_len;
        return i->peek;
    }
    if (transfer.fd >= 0)
        return NULL;
    peek_stop();
    free(i->peek);
    i->peek = NULL;
    strlcpy(i->peek_mime, mime, sizeof i->peek_mime);
    pipe_start(&peek, target_offer, mime);
    return NULL;
}

int gui_transfer_fd(void) { return transfer.fd >= 0 ? transfer.fd : peek.fd; }

/* ---- events ---- */

int gui_next_event(struct wmsg *ev, int timeout_ms)
{
    if (!display)
        return -1;
    for (;;) {
        gui_flush();
        repeat_tick();
        transfer_pump(0);
        peek_pump();
        if (qhead != qtail) {
            *ev = queue[qhead];
            qhead = (qhead + 1) % QUEUE_MAX;
            return 1;
        }
        /* Requests made by listeners (acks, commits) go out before waiting. */
        if (dispatch_events() < 0)
            return -1;
        gui_flush();
        if (qhead != qtail)
            continue;
        if (timeout_ms == 0)
            return 0;
        int wait = timeout_ms, rt = gui_repeat_timeout();
        if (rt >= 0 && (wait < 0 || rt < wait))
            wait = rt;
        struct pollfd pf[2] = { { wire_display_fd(display), POLLIN, 0 }, { gui_transfer_fd(), POLLIN, 0 } };
        int r = poll(pf, pf[1].fd >= 0 ? 2 : 1, wait);
        if (r == 0) {
            if (wait != timeout_ms)
                continue;           /* a repeat is due */
            return 0;
        }
        if (r < 0 && errno != EINTR)
            return -1;
        if (r > 0 && !pf[0].revents)
            continue;               /* drop data arrived */
        if (dispatch_events() < 0)
            return -1;
        if (timeout_ms > 0 && qhead == qtail) {
            gui_flush();
            return 0;
        }
    }
}
