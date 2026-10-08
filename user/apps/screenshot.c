/* screenshot: save the screen, an area or a window as a PNG file through
 * the screencopy interface of the display server.
 *
 *   screenshot [-i | -a | -w] [-p] [-t] [-d SECONDS] [FILE]
 *
 * Without a mode the whole screen is saved at once. -i opens a capture
 * interface modelled on GNOME's. It shows the frozen screen dimmed, a
 * selection that the user can draw, move and resize, and a toolbar with
 * the Selection, Screen and Window modes, the pointer toggle, the capture
 * button and Close. -a selects an area as macOS does: the user drags an
 * area, and releasing the button saves it. Space switches to the window
 * under the pointer. -w saves the active window. -p includes the
 * pointer. -t shows a thumbnail of the result in the corner of the screen
 * for five seconds, and a click on the thumbnail opens the file. -i and
 * -a always show the thumbnail.
 *
 * Without FILE the image is written to $HOME/Pictures/screenshot-DATE-
 * TIME.png, and the directory is created when it does not exist. The
 * image has the device resolution of the screen. A window is drawn alone,
 * with its shadow, on a transparent background. The program prints the
 * path of the file on standard output. A cancelled capture prints nothing
 * and exits with status 1.
 * The display server starts this program on the screenshot keys
 * (docs/design/images.md). */
#include <minios/conf.h>
#include <gui/client.h>
#include <gui/image.h>
#include <gui/i18n.h>
#include <gui/mime.h>
#include <gui/paint.h>
#include <gui/theme.h>
#include <gui/widget.h>
#include <wire/client.h>
#include <core-client.h>
#include <debug-client.h>
#include <errno.h>
#include <locale.h>
#include <minios/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define FORMAT_XRGB8888 1
#define FORMAT_ARGB8888 2
#define MAX_WINDOWS 64
#define THUMB_MS 5000

/* ---- the display server ---- */

struct window_info {
    uint32_t id;
    struct rect frame;          /* logical pixels */
    int activated;
    int image_w, image_h;       /* the window with its shadow, device pixels */
};

static struct wire_display *display;
static struct wire_proxy *screencopy, *shm;
static int width, height, scale = 1;          /* the screen in device pixels */
static int done, failed;
static struct rect pointer_rect;              /* device pixels, from the last capture with the pointer */
static struct window_info windows[MAX_WINDOWS];
static int nwindows, windows_done;

static void on_size(void *user, struct wire_proxy *self, int32_t w, int32_t h, int32_t s)
{
    width = w;
    height = h;
    scale = s > 0 ? s : 1;
}
static void on_done(void *user, struct wire_proxy *self) { done = 1; }
static void on_failed(void *user, struct wire_proxy *self) { failed = 1; }
static void on_pointer(void *user, struct wire_proxy *self, int32_t x, int32_t y, int32_t w, int32_t h)
{
    pointer_rect = (struct rect){ x, y, w, h };
}
static void on_window(void *user, struct wire_proxy *self, uint32_t id, int32_t x, int32_t y, int32_t w, int32_t h,
                      uint32_t activated, const char *title, int32_t image_w, int32_t image_h)
{
    if (nwindows < MAX_WINDOWS && w > 0 && h > 0 && image_w > 0 && image_h > 0)
        windows[nwindows++] = (struct window_info){ id, { x, y, w, h }, activated != 0, image_w, image_h };
}
static void on_windows_done(void *user, struct wire_proxy *self) { windows_done = 1; }
static const struct screencopy_listener screencopy_events = { on_size, on_done, on_failed, on_pointer, on_window,
                                                              on_windows_done };

/* A shared memory buffer the server copies into. */
struct shot_buffer {
    int fd;
    size_t bytes;
    uint32_t *pixels;
    struct wire_proxy *pool, *buffer;
};

static int buffer_create(struct shot_buffer *b, int w, int h, uint32_t format)
{
    b->bytes = (size_t)w * h * 4;
    b->fd = memfd_create("screenshot", MFD_CLOEXEC);
    if (b->fd < 0 || ftruncate(b->fd, (long)b->bytes) < 0)
        return -errno;
    b->pixels = mmap(NULL, b->bytes, PROT_READ | PROT_WRITE, MAP_SHARED, b->fd, 0);
    if (b->pixels == MAP_FAILED)
        return -errno;
    b->pool = shm_create_pool(shm, b->fd, (int32_t)b->bytes);
    b->buffer = shm_pool_create_buffer(b->pool, 0, w, h, w * 4, format);
    return 0;
}

static void buffer_destroy_all(struct shot_buffer *b)
{
    buffer_destroy(b->buffer);
    shm_pool_destroy(b->pool);
    munmap(b->pixels, b->bytes);
    close(b->fd);
}

/* Wait for done or failed after the last request. Returns 0 after done. */
static int wait_copy(void)
{
    while (!done && !failed && wire_display_error(display) == 0)
        if (wire_display_roundtrip(display) < 0)
            break;
    int ok = done;
    done = failed = 0;
    return ok ? 0 : -1;
}

/* ---- the frozen screen ---- */

static struct shot_buffer screen;
static uint32_t *pointer_pixels;              /* the pointer's rectangle as captured with it */

/* The screen without the pointer, and the pointer's rectangle from a
 * capture with it, so that either can be saved later. */
static int freeze(void)
{
    screencopy_capture(screencopy, screen.buffer, 1);
    if (wait_copy() < 0)
        return -1;
    struct rect r = pointer_rect;
    if (r.w > 0 && r.h > 0) {
        pointer_pixels = malloc((size_t)r.w * r.h * 4);
        if (pointer_pixels)
            for (int y = 0; y < r.h; y++)
                memcpy(pointer_pixels + (size_t)y * r.w, screen.pixels + (size_t)(r.y + y) * width + r.x,
                       (size_t)r.w * 4);
    }
    screencopy_capture(screencopy, screen.buffer, 0);
    return wait_copy();
}

/* A device pixel rectangle of the frozen screen, with the pointer when
 * asked and captured. */
static struct image *crop(struct rect r, int with_pointer)
{
    struct image *img = image_create(r.w, r.h);
    if (!img)
        return NULL;
    for (int y = 0; y < r.h; y++)
        memcpy(img->pixels + (size_t)y * r.w, screen.pixels + (size_t)(r.y + y) * width + r.x, (size_t)r.w * 4);
    struct rect p = rect_intersect(pointer_rect, r);
    if (with_pointer && pointer_pixels && !rect_empty(p))
        for (int y = p.y; y < p.y + p.h; y++)
            memcpy(img->pixels + (size_t)(y - r.y) * r.w + (p.x - r.x),
                   pointer_pixels + (size_t)(y - pointer_rect.y) * pointer_rect.w + (p.x - pointer_rect.x),
                   (size_t)p.w * 4);
    return img;
}

static int get_windows(void);

/* One window drawn alone by the server with its shadow, in device pixels
 * with alpha, as on macOS. The list is asked for again first, since the
 * window may have changed its size while the interface was shown. */
static struct image *capture_window(uint32_t id)
{
    const struct window_info *wi = NULL;
    if (get_windows() < 0)
        return NULL;
    for (int i = 0; i < nwindows && !wi; i++)
        if (windows[i].id == id)
            wi = &windows[i];
    if (!wi)
        return NULL;
    int w = wi->image_w, h = wi->image_h;
    struct shot_buffer b;
    if (buffer_create(&b, w, h, FORMAT_ARGB8888) < 0)
        return NULL;
    screencopy_capture_window(screencopy, b.buffer, wi->id);
    struct image *img = wait_copy() == 0 ? image_create(w, h) : NULL;
    if (img)
        memcpy(img->pixels, b.pixels, (size_t)w * h * 4);
    buffer_destroy_all(&b);
    return img;
}

static int get_windows(void)
{
    nwindows = windows_done = 0;
    screencopy_get_windows(screencopy);
    while (!windows_done && wire_display_error(display) == 0)
        if (wire_display_roundtrip(display) < 0)
            return -1;
    return 0;
}

/* ---- the capture interface ---- */

enum mode { MODE_SELECTION, MODE_SCREEN, MODE_WINDOW };
enum { BTN_NONE = -1, BTN_SELECTION, BTN_SCREEN, BTN_WINDOW, BTN_CAPTURE, BTN_POINTER, BTN_CLOSE, BTN_COUNT };
enum { DRAG_NONE, DRAG_NEW, DRAG_MOVE, DRAG_RESIZE };
enum { EDGE_L = 1, EDGE_R = 2, EDGE_T = 4, EDGE_B = 8 };

/* Toolbar geometry in logical pixels. */
#define BAR_PAD 6
#define BAR_GAP 4
#define BAR_SEP 12
#define BAR_BTN_H 44
#define BAR_MODE_W 72
#define BAR_BOTTOM 24
#define HANDLE_R 5
#define HANDLE_HIT 8
#define MIN_SELECTION 4

#define C_WHITE 0x00ffffffu
#define C_OUTLINE 0x00202020u

struct ui {
    struct gui_window *win;
    struct theme theme;
    int sw, sh;                     /* the screen in logical pixels */
    int quick;                      /* -a: no toolbar, the release captures */
    enum mode mode;
    int x0, y0, x1, y1;             /* the selection, edges in any order */
    int drag, edges, ax, ay;        /* drag state, and the press point or the grip inside the selection */
    int started;                    /* a new selection replaces the old one once the pointer moved */
    int px, py;                     /* the pointer */
    int pointer;                    /* save the pointer */
    int hover_btn, press_btn;
    int hover_win, chosen_win;      /* indices into windows[], -1 for none */
    struct rect painted;            /* extent of the marks drawn last, for the next repaint */
    const struct image *icons[BTN_COUNT];
    int result;                     /* 1 capture, -1 cancel, 0 running */
};

static struct rect norm(int x0, int y0, int x1, int y1)
{
    int l = x0 < x1 ? x0 : x1, t = y0 < y1 ? y0 : y1;
    return (struct rect){ l, t, abs(x1 - x0), abs(y1 - y0) };
}

static struct rect selection(const struct ui *u) { return norm(u->x0, u->y0, u->x1, u->y1); }

static struct rect grow(struct rect r, int n) { return (struct rect){ r.x - n, r.y - n, r.w + 2 * n, r.h + 2 * n }; }

static struct rect bar_rect(const struct ui *u)
{
    int w = 2 * BAR_PAD + 3 * BAR_MODE_W + 2 * BAR_GAP + 2 * BAR_SEP + 3 * BAR_BTN_H + BAR_GAP;
    int h = 2 * BAR_PAD + BAR_BTN_H;
    return (struct rect){ (u->sw - w) / 2, u->sh - BAR_BOTTOM - h, w, h };
}

static struct rect button_rect(const struct ui *u, int b)
{
    struct rect bar = bar_rect(u);
    int x = bar.x + BAR_PAD, y = bar.y + BAR_PAD;
    if (b <= BTN_WINDOW)
        return (struct rect){ x + b * (BAR_MODE_W + BAR_GAP), y, BAR_MODE_W, BAR_BTN_H };
    x += 3 * BAR_MODE_W + 2 * BAR_GAP + BAR_SEP;
    if (b == BTN_CAPTURE)
        return (struct rect){ x, y, BAR_BTN_H, BAR_BTN_H };
    x += BAR_BTN_H + BAR_SEP;
    if (b == BTN_POINTER)
        return (struct rect){ x, y, BAR_BTN_H, BAR_BTN_H };
    return (struct rect){ x + BAR_BTN_H + BAR_GAP, y, BAR_BTN_H, BAR_BTN_H };
}

static int button_at(const struct ui *u, int x, int y)
{
    if (u->quick)
        return BTN_NONE;
    for (int b = 0; b < BTN_COUNT; b++)
        if (rect_contains(button_rect(u, b), x, y))
            return b;
    return BTN_NONE;
}

static int window_at(int x, int y)
{
    for (int i = 0; i < nwindows; i++)
        if (rect_contains(windows[i].frame, x, y))
            return i;
    return -1;
}

static const char *size_label(const struct ui *u, char *buf, size_t n)
{
    struct rect s = selection(u);
    snprintf(buf, n, "%d × %d", s.w * scale, s.h * scale);
    return buf;
}

/* The size label: below the selection, above it when there is no room. */
static struct rect label_rect(const struct ui *u, struct painter *p)
{
    char buf[32];
    struct rect s = selection(u);
    int w = painter_text_width(p, size_label(u, buf, sizeof buf), -1) + 16;
    int h = painter_text_height(p) + 8;
    int y = s.y + s.h + 10;
    if (y + h > u->sh)
        y = s.y - h - 10 >= 0 ? s.y - h - 10 : s.y + s.h - h - 10;
    return (struct rect){ s.x + (s.w - w) / 2, y, w, h };
}

/* The rectangle shown at full brightness, and the extent of every mark
 * drawn around it: the parts that change with the state. */
static struct rect marks_extent(const struct ui *u, struct painter *p)
{
    switch (u->mode) {
    case MODE_SCREEN:
        return (struct rect){ 0, 0, u->sw, u->sh };
    case MODE_WINDOW: {
        struct rect r = { 0, 0, 0, 0 };
        if (u->hover_win >= 0)
            r = grow(windows[u->hover_win].frame, 3);
        if (u->chosen_win >= 0)
            r = rect_union(r, grow(windows[u->chosen_win].frame, 3));
        return r;
    }
    default:
        if (u->drag == DRAG_NONE && rect_empty(selection(u)))
            return (struct rect){ 0, 0, 0, 0 };
        return rect_union(grow(selection(u), HANDLE_R + 2), grow(label_rect(u, p), 1));
    }
}

/* An antialiased disc of radius r logical pixels centred at (cx, cy),
 * with a ring of colour ring at its edge one logical pixel wide. */
static void disc(struct painter *p, int cx, int cy, int r, uint32_t fill, uint32_t ring)
{
    painter_disc(p, cx - r, cy - r, 2 * r, ring);
    if (fill != ring)
        painter_disc(p, cx - r + 1, cy - r + 1, 2 * r - 2, fill);
}

static void frame(struct painter *p, struct rect r, int n, uint32_t c)
{
    for (int i = 0; i < n; i++)
        painter_frame(p, r.x - 1 - i, r.y - 1 - i, r.w + 2 + 2 * i, r.h + 2 + 2 * i, c);
}

static void draw_icon(struct painter *p, const struct image *icon, struct rect area)
{
    if (icon)
        painter_image(p, area.x + (area.w - image_lw(icon)) / 2, area.y + (area.h - image_lh(icon)) / 2, icon);
}

/* The toolbar in the colours of the theme: the mode buttons with an icon
 * over the label, the capture button as an accent ring around an accent
 * disc, the pointer toggle and Close. */
static void draw_toolbar(struct ui *u, struct painter *p)
{
    static const char *const labels[3] = { N_("Selection"), N_("Screen"), N_("Window") };
    const uint32_t *c = p->theme->color;
    struct rect bar = bar_rect(u);
    painter_round_rect(p, bar.x, bar.y, bar.w, bar.h, 8, c[TC_WINDOW], c[TC_BORDER]);
    for (int b = 0; b < BTN_COUNT; b++) {
        struct rect r = button_rect(u, b);
        int on = (b <= BTN_WINDOW && (int)u->mode == b) || (b == BTN_POINTER && u->pointer);
        int hot = u->hover_btn == b;
        if (b == BTN_CAPTURE) {
            int cx = r.x + r.w / 2, cy = r.y + r.h / 2, R = r.w / 2 - 2;
            uint32_t core = hot ? c[TC_SELECTION] : c[TC_ACCENT];
            disc(p, cx, cy, R, c[TC_ACCENT], c[TC_ACCENT]);
            disc(p, cx, cy, R - 2, c[TC_WINDOW], c[TC_WINDOW]);
            disc(p, cx, cy, R - 5, core, core);
            continue;
        }
        if (on || hot)
            painter_round_rect(p, r.x, r.y, r.w, r.h, 6, on ? c[TC_BUTTON_PRESSED] : c[TC_BUTTON_HOVER],
                               on ? c[TC_BORDER] : c[TC_BUTTON_HOVER]);
        if (b <= BTN_WINDOW) {
            const char *text = _(labels[b]);
            int th = painter_text_height(p);
            draw_icon(p, u->icons[b], (struct rect){ r.x, r.y + 3, r.w, r.h - th - 4 });
            int tw = painter_text_width(p, text, -1);
            painter_text(p, r.x + (r.w - tw) / 2, r.y + r.h - th - 2, text, c[TC_TEXT]);
        } else {
            draw_icon(p, u->icons[b], r);
        }
    }
}

static void draw_marks(struct ui *u, struct painter *p)
{
    if (u->mode == MODE_SELECTION && !rect_empty(selection(u))) {
        struct rect sel = selection(u);
        frame(p, sel, 1, C_WHITE);
        int xs[3] = { sel.x, sel.x + sel.w / 2, sel.x + sel.w }, ys[3] = { sel.y, sel.y + sel.h / 2, sel.y + sel.h };
        if (!u->quick)
            for (int j = 0; j < 3; j++)
                for (int i = 0; i < 3; i++)
                    if ((i != 1 || j != 1) && ((i != 1 && j != 1) || (sel.w > 40 && sel.h > 40)))
                        disc(p, xs[i], ys[j], HANDLE_R, C_WHITE, C_OUTLINE);
        char buf[32];
        struct rect l = label_rect(u, p);
        painter_round_rect(p, l.x, l.y, l.w, l.h, 4, p->theme->color[TC_WINDOW], p->theme->color[TC_BORDER]);
        painter_text(p, l.x + 8, l.y + 4, size_label(u, buf, sizeof buf), p->theme->color[TC_TEXT]);
    } else if (u->mode == MODE_WINDOW) {
        if (u->hover_win >= 0 && u->hover_win != u->chosen_win)
            frame(p, windows[u->hover_win].frame, 1, C_WHITE);
        if (u->chosen_win >= 0)
            frame(p, windows[u->chosen_win].frame, 2, p->theme->color[TC_ACCENT]);
    }
}

/* Repaint a logical rectangle: the dimmed frozen screen, the bright part,
 * the marks and the toolbar, clipped to it. */
static void repaint(struct ui *u, struct rect r)
{
    r = rect_intersect(r, (struct rect){ 0, 0, u->sw, u->sh });
    if (rect_empty(r))
        return;
    gui_begin_paint(u->win);
    int S = scale;
    struct surface *s = &u->win->surf;
    struct rect R = { r.x * S, r.y * S, r.w * S, r.h * S };
    for (int y = R.y; y < R.y + R.h; y++) {
        const uint32_t *from = screen.pixels + (size_t)y * width;
        uint32_t *to = s->pixels + (size_t)y * s->stride;
        for (int x = R.x; x < R.x + R.w; x++)
            to[x] = (from[x] >> 1) & 0x007f7f7fu;
    }
    struct rect bright = { 0, 0, 0, 0 };
    if (u->mode == MODE_SELECTION)
        bright = selection(u);
    else if (u->mode == MODE_SCREEN)
        bright = (struct rect){ 0, 0, u->sw, u->sh };
    else if (u->chosen_win >= 0)
        bright = windows[u->chosen_win].frame;
    else if (u->hover_win >= 0)
        bright = windows[u->hover_win].frame;
    bright = rect_intersect(bright, r);
    for (int y = bright.y * S; y < (bright.y + bright.h) * S; y++)
        memcpy(s->pixels + (size_t)y * s->stride + bright.x * S, screen.pixels + (size_t)y * width + bright.x * S,
               (size_t)bright.w * S * 4);
    struct painter p;
    painter_init_scaled(&p, s, &u->theme, S);
    p.clip = R;
    draw_marks(u, &p);
    if (!u->quick)
        draw_toolbar(u, &p);
    gui_damage(u->win, r.x, r.y, r.w, r.h);
}

/* After a change of state: the old and the new marks and the toolbar. */
static void refresh(struct ui *u)
{
    struct painter p;
    painter_init_scaled(&p, &u->win->surf, &u->theme, scale);
    struct rect now = marks_extent(u, &p);
    struct rect r = rect_union(u->painted, now);
    if (!u->quick)
        r = rect_union(r, bar_rect(u));
    repaint(u, r);
    u->painted = now;
}

static void set_mode(struct ui *u, enum mode m)
{
    if (u->mode == m)
        return;
    u->mode = m;
    u->drag = DRAG_NONE;
    if (m == MODE_WINDOW && u->chosen_win < 0 && !u->quick) {
        for (int i = 0; i < nwindows && u->chosen_win < 0; i++)
            if (windows[i].activated)
                u->chosen_win = i;
        if (u->chosen_win < 0 && nwindows)
            u->chosen_win = 0;
    }
    u->hover_win = m == MODE_WINDOW ? window_at(u->px, u->py) : -1;
    repaint(u, (struct rect){ 0, 0, u->sw, u->sh });
    struct painter p;
    painter_init_scaled(&p, &u->win->surf, &u->theme, scale);
    u->painted = marks_extent(u, &p);
}

/* The capture button, Enter, Space and Print Screen end the interface
 * when there is something to save. */
static void try_capture(struct ui *u)
{
    if (u->mode == MODE_SELECTION && (selection(u).w < 1 || selection(u).h < 1))
        return;
    if (u->mode == MODE_WINDOW && u->chosen_win < 0 && u->hover_win < 0)
        return;
    if (u->mode == MODE_WINDOW && u->chosen_win < 0)
        u->chosen_win = u->hover_win;
    u->result = 1;
}

/* Which edges of the selection a press at (x, y) grabs. */
static int edges_at(const struct ui *u, int x, int y)
{
    struct rect s = selection(u);
    if (rect_empty(s) || u->quick)
        return 0;
    int e = 0;
    int in_x = x >= s.x - HANDLE_HIT && x <= s.x + s.w + HANDLE_HIT;
    int in_y = y >= s.y - HANDLE_HIT && y <= s.y + s.h + HANDLE_HIT;
    if (in_y && abs(x - s.x) <= HANDLE_HIT) e |= EDGE_L;
    else if (in_y && abs(x - (s.x + s.w)) <= HANDLE_HIT) e |= EDGE_R;
    if (in_x && abs(y - s.y) <= HANDLE_HIT) e |= EDGE_T;
    else if (in_x && abs(y - (s.y + s.h)) <= HANDLE_HIT) e |= EDGE_B;
    return e;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static void press(struct ui *u, int x, int y)
{
    int b = button_at(u, x, y);
    if (b != BTN_NONE) {
        u->press_btn = b;
        return;
    }
    if (u->mode == MODE_WINDOW) {
        int i = window_at(x, y);
        if (i >= 0) {
            u->chosen_win = i;
            if (u->quick)
                u->result = 1;
        }
        refresh(u);
        return;
    }
    if (u->mode != MODE_SELECTION)
        return;
    struct rect s = selection(u);
    int e = edges_at(u, x, y);
    if (e) {
        /* The edges under the press follow the pointer, and the others
         * remain in place. */
        u->x0 = s.x;
        u->y0 = s.y;
        u->x1 = s.x + s.w;
        u->y1 = s.y + s.h;
        u->drag = DRAG_RESIZE;
        u->edges = e;
    } else if (!u->quick && rect_contains(s, x, y)) {
        u->drag = DRAG_MOVE;
        u->ax = x - s.x;
        u->ay = y - s.y;
    } else {
        u->drag = DRAG_NEW;
        u->started = 0;
        u->ax = x;
        u->ay = y;
    }
}

static void motion(struct ui *u, int x, int y)
{
    x = clampi(x, 0, u->sw);
    y = clampi(y, 0, u->sh);
    u->px = x;
    u->py = y;
    int changed = 0;
    int b = u->press_btn == BTN_NONE && u->drag == DRAG_NONE ? button_at(u, x, y) : u->hover_btn;
    if (b != u->hover_btn) {
        u->hover_btn = b;
        changed = 1;
    }
    if (u->mode == MODE_WINDOW) {
        int i = b == BTN_NONE ? window_at(x, y) : -1;
        if (i != u->hover_win) {
            u->hover_win = i;
            changed = 1;
        }
    }
    struct rect s = selection(u);
    switch (u->drag) {
    case DRAG_NEW:
        /* A new selection starts once the pointer moved a few pixels, so
         * that a click retains the old one. */
        if (abs(x - u->ax) >= MIN_SELECTION || abs(y - u->ay) >= MIN_SELECTION)
            u->started = 1;
        if (u->started) {
            u->x0 = u->ax;
            u->y0 = u->ay;
            u->x1 = x;
            u->y1 = y;
            changed = 1;
        }
        break;
    case DRAG_MOVE:
        u->x0 = clampi(x - u->ax, 0, u->sw - s.w);
        u->y0 = clampi(y - u->ay, 0, u->sh - s.h);
        u->x1 = u->x0 + s.w;
        u->y1 = u->y0 + s.h;
        changed = 1;
        break;
    case DRAG_RESIZE:
        if (u->edges & EDGE_L) u->x0 = x;
        if (u->edges & EDGE_R) u->x1 = x;
        if (u->edges & EDGE_T) u->y0 = y;
        if (u->edges & EDGE_B) u->y1 = y;
        changed = 1;
        break;
    }
    if (changed)
        refresh(u);
}

static void release(struct ui *u, int x, int y)
{
    int b = u->press_btn;
    u->press_btn = BTN_NONE;
    if (b != BTN_NONE) {
        if (button_at(u, x, y) != b)
            return;
        if (b <= BTN_WINDOW)
            set_mode(u, (enum mode)b);
        else if (b == BTN_CAPTURE)
            try_capture(u);
        else if (b == BTN_POINTER) {
            u->pointer = !u->pointer;
            refresh(u);
        } else
            u->result = -1;
        return;
    }
    int was = u->drag;
    u->drag = DRAG_NONE;
    if (was == DRAG_NONE)
        return;
    /* Store the selection with ordered edges. */
    struct rect s = selection(u);
    u->x0 = s.x;
    u->y0 = s.y;
    u->x1 = s.x + s.w;
    u->y1 = s.y + s.h;
    if (u->quick && was == DRAG_NEW && s.w >= MIN_SELECTION && s.h >= MIN_SELECTION)
        u->result = 1;
    refresh(u);
}

static void key(struct ui *u, int code)
{
    switch (code) {
    case KEY_ESC:
        u->result = -1;
        break;
    case KEY_ENTER:
    case KEY_KPENTER:
    case KEY_SYSRQ:
        try_capture(u);
        break;
    case KEY_SPACE:
        if (u->quick)
            set_mode(u, u->mode == MODE_WINDOW ? MODE_SELECTION : MODE_WINDOW);
        else
            try_capture(u);
        break;
    case KEY_S:
        if (!u->quick)
            set_mode(u, MODE_SELECTION);
        break;
    case KEY_C:
        if (!u->quick)
            set_mode(u, MODE_SCREEN);
        break;
    case KEY_W:
        if (!u->quick)
            set_mode(u, MODE_WINDOW);
        break;
    case KEY_P:
        if (!u->quick) {
            u->pointer = !u->pointer;
            refresh(u);
        }
        break;
    }
}

/* Run the interface on the frozen screen. Returns 1 with the choice in
 * *u, or -1 when cancelled. */
static int run_ui(struct ui *u, int quick, int pointer)
{
    u->sw = width / scale;
    u->sh = height / scale;
    u->quick = quick;
    u->pointer = pointer;
    u->hover_btn = u->press_btn = BTN_NONE;
    u->hover_win = u->chosen_win = -1;
    u->drag = DRAG_NONE;
    u->mode = MODE_SELECTION;
    if (!quick) {
        /* GNOME starts with an area in the middle of the screen. */
        u->x0 = u->sw / 4;
        u->y0 = u->sh / 4;
        u->x1 = u->sw - u->sw / 4;
        u->y1 = u->sh - u->sh / 4;
    }
    if (pointer_rect.w > 0) {
        u->px = pointer_rect.x / scale;
        u->py = pointer_rect.y / scale;
    }
    theme_init_default(&u->theme);
    theme_apply(&u->theme);
    static const char *const names[BTN_COUNT] = { "rectangle", "wallpaper", "app-default", NULL, "app-evtest", "quit" };
    for (int b = 0; b < BTN_COUNT; b++)
        if (names[b])
            u->icons[b] = icon_lookup(names[b], b <= BTN_WINDOW ? 16 : 18, scale, u->theme.color[TC_TEXT] & 0x00ffffffu);
    u->win = gui_create_layer_window(u->sw, u->sh, 3, GUI_ANCHOR_TOP | GUI_ANCHOR_LEFT, 0, 1, "screenshot");
    if (!u->win || u->win->scale != scale)
        return -1;
    repaint(u, (struct rect){ 0, 0, u->sw, u->sh });
    struct painter p;
    painter_init_scaled(&p, &u->win->surf, &u->theme, scale);
    u->painted = marks_extent(u, &p);
    struct wmsg ev;
    while (!u->result && gui_next_event(&ev, -1) > 0) {
        if (ev.type == WM_CLOSE) {
            u->result = -1;
        } else if (ev.type == WM_KEY && ev.b) {
            key(u, ev.a);
        } else if (ev.type == WM_MOUSE) {
            if (ev.d == WMOUSE_DOWN && (ev.c & 1))
                press(u, ev.a, ev.b);
            else if (ev.d == WMOUSE_UP && !(ev.c & 1))
                release(u, ev.a, ev.b);
            else if (ev.d == WMOUSE_MOVE)
                motion(u, ev.a, ev.b);
        }
    }
    gui_destroy_window(u->win);
    gui_flush();
    wire_display_roundtrip(display);
    theme_release(&u->theme);
    return u->result > 0 ? 1 : -1;
}

/* ---- the thumbnail ---- */

/* The saved image in the bottom right corner of the desktop area, as on
 * macOS: a click opens the file, otherwise it goes after five seconds,
 * counted again whenever the pointer moves over it. */
static void thumbnail(const struct image *img, const char *path)
{
    int maxw = 220, maxh = 140;
    int lw = img->w / scale, lh = img->h / scale;
    int tw = lw, th = lh;
    if (tw > maxw) {
        th = th * maxw / tw;
        tw = maxw;
    }
    if (th > maxh) {
        tw = tw * maxh / th;
        th = maxh;
    }
    if (tw < 1) tw = 1;
    if (th < 1) th = 1;
    struct image *small = image_scale(img, tw * scale, th * scale);
    struct gui_window *w = small ? gui_create_layer_window(tw + 2, th + 2, 3, GUI_ANCHOR_BOTTOM | GUI_ANCHOR_RIGHT, 0, 0,
                                                           "screenshot-thumbnail") : NULL;
    if (!w) {
        if (small)
            image_free(small);
        return;
    }
    gui_layer_set_margin(w, 0, 16, 16, 0);
    small->scale = scale;
    struct theme theme;
    theme_init_default(&theme);
    struct painter p;
    gui_begin_paint(w);
    painter_init_scaled(&p, &w->surf, &theme, w->scale);
    painter_fill(&p, 0, 0, tw + 2, th + 2, 0x00303030);
    painter_image(&p, 1, 1, small);
    painter_frame(&p, 0, 0, tw + 2, th + 2, 0x00c0c0c0);
    gui_damage(w, 0, 0, tw + 2, th + 2);
    long left = THUMB_MS;
    struct wmsg ev;
    while (left > 0) {
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        int r = gui_next_event(&ev, (int)left);
        if (r < 0)
            break;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        left -= (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
        if (r == 0)
            continue;
        if (ev.type == WM_MOUSE && ev.d == WMOUSE_MOVE)
            left = THUMB_MS;
        else if (ev.type == WM_MOUSE && ev.d == WMOUSE_UP && ev.window == w->id) {
            mime_open(path);
            break;
        } else if (ev.type == WM_CLOSE) {
            break;
        }
    }
    gui_destroy_window(w);
    image_free(small);
}

/* ---- main ---- */

/* $HOME/Pictures/screenshot-YYYY-MM-DD-HHMMSS.png, with -2, -3 and so on
 * appended when a file of that name exists. */
static int default_path(char *path, size_t size)
{
    const char *home = getenv("HOME");
    char dir[200];
    snprintf(dir, sizeof dir, "%s/Pictures", home && home[0] ? home : conf_home());
    if (mkdir(dir, 0755) < 0 && errno != EEXIST)
        return -errno;
    time_t now = time(NULL);
    struct tm tm;
    char stamp[32];
    strftime(stamp, sizeof stamp, "%Y-%m-%d-%H%M%S", localtime_r(&now, &tm));
    struct stat st;
    for (int n = 1; n < 100; n++) {
        if (n == 1)
            snprintf(path, size, "%s/screenshot-%s.png", dir, stamp);
        else
            snprintf(path, size, "%s/screenshot-%s-%d.png", dir, stamp, n);
        if (stat(path, &st) < 0)
            return 0;
    }
    return -EEXIST;
}

static void usage(void)
{
    fprintf(stderr, "usage: screenshot [-i | -a | -w] [-p] [-t] [-d SECONDS] [FILE]\n");
    exit(2);
}

static int fail(const char *what)
{
    fprintf(stderr, "screenshot: %s\n", what);
    return 1;
}

int main(int argc, char **argv)
{
    setlocale(LC_ALL, "");
    textdomain("screenshot");
    int delay = 0, interactive = 0, area = 0, window = 0, pointer = 0, thumb = 0;
    const char *file = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
            delay = atoi(argv[++i]);
        else if (strcmp(argv[i], "-i") == 0)
            interactive = 1;
        else if (strcmp(argv[i], "-a") == 0)
            area = 1;
        else if (strcmp(argv[i], "-w") == 0)
            window = 1;
        else if (strcmp(argv[i], "-p") == 0)
            pointer = 1;
        else if (strcmp(argv[i], "-t") == 0)
            thumb = 1;
        else if (argv[i][0] == '-')
            usage();
        else if (!file)
            file = argv[i];
        else
            usage();
    }
    if (interactive + area + window > 1)
        usage();
    if (interactive || area)
        thumb = 1;
    if (delay > 0)
        sleep((unsigned)delay);
    if (gui_connect() < 0) {
        fprintf(stderr, "screenshot: cannot connect to the display server: %s\n", strerror(errno));
        return 1;
    }
    display = gui_display();
    screencopy = gui_bind_global("screencopy", &screencopy_interface, 1);
    shm = gui_bind_global("shm", &shm_interface, 1);
    if (!screencopy || !shm)
        return fail("the display server has no screencopy interface");
    screencopy_add_listener(screencopy, &screencopy_events, NULL);
    wire_display_roundtrip(display);
    if (width <= 0 || height <= 0)
        return fail("no screen size from the display server");

    struct image *img = NULL;
    if (window) {
        if (get_windows() < 0)
            return fail("no window list from the display server");
        int pick = -1;
        for (int i = 0; i < nwindows && pick < 0; i++)
            if (windows[i].activated)
                pick = i;
        if (pick < 0 && nwindows)
            pick = 0;
        if (pick < 0)
            return fail("there is no window");
        img = capture_window(windows[pick].id);
        if (!img)
            return fail("the display server did not copy the window");
    } else {
        int err = buffer_create(&screen, width, height, FORMAT_XRGB8888);
        if (err < 0) {
            fprintf(stderr, "screenshot: cannot allocate the screen buffer: %s\n", strerror(-err));
            return 1;
        }
        if (interactive || area) {
            if (freeze() < 0 || get_windows() < 0)
                return fail("the display server did not copy the screen");
            struct ui u = { 0 };
            if (run_ui(&u, area, pointer) < 0)
                return 1;
            if (u.mode == MODE_WINDOW) {
                img = capture_window(windows[u.chosen_win].id);
            } else {
                struct rect r = u.mode == MODE_SCREEN ? (struct rect){ 0, 0, u.sw, u.sh } : selection(&u);
                r = rect_intersect(r, (struct rect){ 0, 0, u.sw, u.sh });
                img = crop((struct rect){ r.x * scale, r.y * scale, r.w * scale, r.h * scale }, u.pointer);
            }
        } else {
            screencopy_capture(screencopy, screen.buffer, (uint32_t)pointer);
            if (wait_copy() < 0)
                return fail("the display server did not copy the screen");
            img = crop((struct rect){ 0, 0, width, height }, 0);
        }
        buffer_destroy_all(&screen);
        if (!img)
            return fail("cannot copy the image");
    }

    char path[256];
    if (file) {
        strlcpy(path, file, sizeof path);
    } else {
        int err = default_path(path, sizeof path);
        if (err < 0) {
            fprintf(stderr, "screenshot: no file name: %s\n", strerror(-err));
            return 1;
        }
    }
    int err = image_save_png(img, path);
    if (err < 0) {
        fprintf(stderr, "screenshot: %s: %s\n", path, strerror(-err));
        return 1;
    }
    printf("%s\n", path);
    fflush(stdout);
    if (thumb)
        thumbnail(img, path);
    image_free(img);
    screencopy_destroy(screencopy);
    wire_display_roundtrip(display);
    gui_disconnect();
    return 0;
}
