/* Host replacement for client.c: windows are plain buffers, damage is
 * recorded, and events come from a queue the tests fill. */
#include <gui/client.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "fake.h"

static int next_id = 1;
static struct wmsg queue[256];
static int qhead, qtail;
struct rect fake_last_damage;
int fake_damage_count;
static char clip[WSRV_CLIP_MAX];
static int clip_len;

long uptime_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

long uptime_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000000 + tv.tv_usec;
}

void fake_push(const struct wmsg *m)
{
    queue[qtail] = *m;
    qtail = (qtail + 1) % 256;
}

int gui_connect(void) { return 0; }
void gui_disconnect(void) {}
int gui_screen_width(void) { return 1024; }
int gui_screen_height(void) { return 768; }
int gui_output_count(void) { return 1; }
int gui_get_output(int index, struct gui_output_info *out)
{
    if (index || !out) return -1;
    *out = (struct gui_output_info){ 0, 0, 1024, 768, 1, 0, 60000 };
    return 0;
}

static void alloc_surfaces(struct gui_window *w, int width, int height)
{
    free(w->surf.pixels);
    w->width = width;
    w->height = height;
    w->scale = 1;
    w->surf = (struct surface){ calloc((size_t)width * height, 4), width, height, width };
}

struct gui_window *gui_create_window(int width, int height, const char *title)
{
    struct gui_window *w = calloc(1, sizeof *w);
    w->id = next_id++;
    alloc_surfaces(w, width, height);
    return w;
}

struct gui_window *gui_create_dialog_window(struct gui_window *parent, int width, int height, const char *title)
{
    return gui_create_window(width, height, title);
}

int gui_has_popup_surfaces(void) { return 0; }
void gui_text_input_set(struct gui_window *window, int enabled) { (void)window; (void)enabled; }
void gui_text_input_set_cursor(struct gui_window *window, int x, int y, int width, int height)
{
    (void)window; (void)x; (void)y; (void)width; (void)height;
}

struct gui_window *gui_create_popup_window(struct gui_window *parent, int x, int y, int width, int height, int grab)
{
    return NULL;
}

struct gui_window *gui_create_layer_window(int width, int height, int layer, int anchor, int exclusive,
                                           int keyboard, const char *ns)
{
    return gui_create_window(width > 0 ? width : 1024, height > 0 ? height : 740, ns);
}

void gui_destroy_window(struct gui_window *w)
{
    free(w->surf.pixels);
    free(w);
}

void gui_damage(struct gui_window *w, int x, int y, int width, int height)
{
    struct rect r = { x, y, width, height };
    fake_last_damage = r;
    fake_damage_count++;
}
void gui_set_opaque_region(struct gui_window *w, const struct rect *r, int n) { (void)w; (void)r; (void)n; }
void gui_set_input_region(struct gui_window *w, const struct rect *r, int n) { (void)w; (void)r; (void)n; }

void gui_flush(void) {}

static struct gui_stats fake_stats;
void gui_get_stats(struct gui_stats *out) { *out = fake_stats; }
void gui_count_paint(long us)
{
    fake_stats.paints++;
    fake_stats.paint_us += (uint64_t)us;
}

void gui_move(struct gui_window *w, int x, int y) {}
void gui_set_title(struct gui_window *w, const char *title) {}
void gui_set_min_size(struct gui_window *w, int width, int height) {}

void gui_resize(struct gui_window *w, int width, int height)
{
    alloc_surfaces(w, width, height);
    struct wmsg m = { .type = WM_RESIZED, .window = w->id, .a = width, .b = height };
    fake_push(&m);
}

int gui_clipboard_set(const char *text, int len)
{
    if (len > WSRV_CLIP_MAX) len = WSRV_CLIP_MAX;
    memcpy(clip, text, (size_t)len);
    clip_len = len;
    return 0;
}

int gui_clipboard_get(char *buf, int size)
{
    int n = clip_len < size ? clip_len : size;
    memcpy(buf, clip, (size_t)n);
    if (n < size)
        buf[n] = '\0';
    return n;
}

int gui_next_event(struct wmsg *ev, int timeout_ms)
{
    if (qhead == qtail)
        return 0;
    *ev = queue[qhead];
    qhead = (qhead + 1) % 256;
    return 1;
}

int gui_event_fd(void) { return -1; }

/* No key repeat in the fake client. */
int gui_repeat_timeout(void) { return 0; }

/* Drag and drop: a drag the program starts is recorded, and the tests
 * set the types and the data of a drag over a window and of a drop. */
struct fake_drag fake_drag;
const char *fake_offers[8];
const char *fake_peek;
const char *fake_accept_mime;
int fake_accept_actions, fake_accept_preferred;
const char *fake_drop, *fake_drop_mime;

int gui_drag_start(struct gui_window *w, const struct gui_drag_item *items, int nitems, int actions,
                   const struct surface *icon, int hot_x, int hot_y)
{
    if (!w || nitems <= 0 || nitems > 8)
        return -1;
    fake_drag.started++;
    fake_drag.window = w->id;
    fake_drag.nitems = nitems;
    fake_drag.actions = actions;
    fake_drag.icon = icon && icon->pixels;
    for (int i = 0; i < nitems; i++) {
        snprintf(fake_drag.mime[i], sizeof fake_drag.mime[i], "%s", items[i].mime);
        snprintf(fake_drag.data[i], sizeof fake_drag.data[i], "%.*s", (int)items[i].len, (const char *)items[i].data);
    }
    return 0;
}

int gui_dragging(void) { return fake_drag.started > 0; }

int gui_drag_offers(const char *mime)
{
    for (int i = 0; i < 8 && fake_offers[i]; i++)
        if (strcmp(fake_offers[i], mime) == 0)
            return 1;
    return 0;
}

void gui_drag_accept(const char *mime, int actions, int preferred)
{
    fake_accept_mime = mime;
    fake_accept_actions = actions;
    fake_accept_preferred = preferred;
}

const char *gui_drag_peek(const char *mime, size_t *len)
{
    if (!fake_peek || !gui_drag_offers(mime))
        return NULL;
    if (len)
        *len = strlen(fake_peek);
    return fake_peek;
}

const char *gui_drop_data(size_t *len, const char **mime)
{
    if (len)
        *len = fake_drop ? strlen(fake_drop) : 0;
    if (mime)
        *mime = fake_drop_mime ? fake_drop_mime : "";
    return fake_drop;
}

int gui_transfer_fd(void) { return -1; }
