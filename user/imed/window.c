/* The candidate window of imed (I1 and I2, docs/design/ime.md): the page of
 * the lookup table that contains the cursor, with the auxiliary line above
 * it, drawn with the libgui painter on the candidate surface.  The
 * compositor places the surface below the caret. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <wire/client.h>
#include <gui/paint.h>
#include <gui/theme.h>
#include "core-client.h"
#include "ime-client.h"
#include "imed.h"

#define PAD 6
#define GAP 10                  /* between the candidates of a horizontal page */
#define MAX_PAGE 10

static struct wire_proxy *surface, *role, *pool, *buffer;
static void *pixels;
static size_t mapped;
static int fd = -1, buf_w, buf_h, scale = 1, shown;
static struct theme ui;
static struct rect items[MAX_PAGE];         /* the candidates of the page, in logical pixels */
static int first, count;                    /* the first candidate of the page and their number */
static struct rect prev_arrow, next_arrow;

int window_owns(const struct wire_proxy *s)
{
    return s && s == surface;
}

static void on_placed(void *user, struct wire_proxy *p, int32_t x, int32_t y) {}
static const struct candidate_surface_listener role_events = { on_placed };

void window_init(void)
{
    theme_init_default(&ui);
    ui.metric[TM_FONT_PX] = 16;
    theme_apply(&ui);
    surface = compositor_create_surface(compositor);
    role = input_method_get_candidate_surface(im, surface);
    candidate_surface_add_listener(role, &role_events, NULL);
}

void window_set_scale(int s)
{
    if (s == scale)
        return;
    scale = s;
    if (shown)
        window_update(1);
}

static void release_buffer(void)
{
    if (buffer)
        buffer_destroy(buffer);
    if (pool)
        shm_pool_destroy(pool);
    if (pixels)
        munmap(pixels, mapped);
    if (fd >= 0)
        close(fd);
    buffer = pool = NULL;
    pixels = NULL;
    fd = -1;
}

static int alloc_buffer(int w, int h)
{
    if (buffer && w == buf_w && h == buf_h)
        return 0;
    release_buffer();
    int dw = w * scale, dh = h * scale;
    mapped = (size_t)dw * dh * 4;
    fd = memfd_create("candidates", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (long)mapped) < 0)
        return -1;
    pixels = mmap(NULL, mapped, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pixels == MAP_FAILED) {
        pixels = NULL;
        return -1;
    }
    pool = shm_create_pool(shm, fd, (int32_t)mapped);
    buffer = shm_pool_create_buffer(pool, 0, dw, dh, dw * 4, 1);
    buf_w = w;
    buf_h = h;
    return 0;
}

/* The label of a candidate: its number on the page. */
static void label_of(int i, char *out, size_t size)
{
    snprintf(out, size, "%d.", (i - first) % 10 + 1);
}

static int text_w(struct painter *p, const char *s)
{
    return painter_text_width(p, s, -1);
}

/* layout measures the page and fills items; it returns the size. */
static void layout(struct painter *p, int *w, int *h)
{
    int ps = imed_table.page_size > 0 && imed_table.page_size <= MAX_PAGE ? imed_table.page_size : 5;
    first = imed_table.cursor / ps * ps;
    count = imed_table.n - first < ps ? imed_table.n - first : ps;
    int th = painter_text_height(p), row = th + 6;
    int y = PAD, width = 0;
    if (imed_table.aux[0]) {
        width = text_w(p, imed_table.aux);
        y += row;
    }
    int pages = (imed_table.n + ps - 1) / ps, arrows = pages > 1 ? text_w(p, " ‹ › ") : 0;
    char label[8];
    int x = PAD;
    for (int i = 0; i < count; i++) {
        int k = first + i;
        label_of(k, label, sizeof label);
        int iw = text_w(p, label) + 4 + text_w(p, imed_table.text[k]) +
                 (imed_table.comment[k][0] ? 6 + text_w(p, imed_table.comment[k]) : 0) + 8;
        if (imed_table.vertical) {
            items[i] = (struct rect){ PAD - 2, y, iw, row };
            if (iw > width)
                width = iw;
            y += row;
        } else {
            items[i] = (struct rect){ x - 2, y, iw, row };
            x += iw + GAP;
        }
    }
    if (!imed_table.vertical) {
        if (x - GAP - PAD > width)
            width = x - GAP - PAD;
        y += row;
    }
    width += arrows;
    if (imed_table.vertical)
        for (int i = 0; i < count; i++)
            items[i].w = width;
    *w = width + 2 * PAD;
    *h = y + PAD - 2;
    int ax = *w - PAD - arrows, ay = imed_table.vertical ? *h - PAD - row : y - row;
    prev_arrow = pages > 1 ? (struct rect){ ax, ay, arrows / 2, row } : (struct rect){ 0, 0, 0, 0 };
    next_arrow = pages > 1 ? (struct rect){ ax + arrows / 2, ay, arrows - arrows / 2, row } : (struct rect){ 0, 0, 0, 0 };
}

static void draw(void)
{
    struct surface measure = { .pixels = NULL, .width = 1, .height = 1, .stride = 1 };
    struct painter p;
    painter_init_scaled(&p, &measure, &ui, scale);
    int w, h;
    layout(&p, &w, &h);
    if (alloc_buffer(w, h) < 0)
        return;
    struct surface target = { .pixels = pixels, .width = w * scale, .height = h * scale, .stride = w * scale };
    painter_init_scaled(&p, &target, &ui, scale);
    const uint32_t *c = ui.color;
    painter_fill(&p, 0, 0, w, h, c[TC_WINDOW]);
    painter_rounded(&p, 0, 0, w, h, c[TC_FIELD], c[TC_BORDER]);
    int th = painter_text_height(&p);
    if (imed_table.aux[0])
        painter_text(&p, PAD, PAD + 3, imed_table.aux, c[TC_TEXT]);
    char label[8];
    for (int i = 0; i < count; i++) {
        int k = first + i;
        struct rect r = items[i];
        int selected = k == imed_table.cursor;
        uint32_t fg = selected ? c[TC_SELECTION_TEXT] : c[TC_TEXT];
        if (selected)
            painter_rounded(&p, r.x, r.y, r.w, r.h, c[TC_SELECTION], 0xffffffffu);
        int x = r.x + 4, y = r.y + (r.h - th) / 2;
        label_of(k, label, sizeof label);
        painter_text(&p, x, y, label, selected ? fg : c[TC_TEXT_DISABLED]);
        x += text_w(&p, label) + 4;
        painter_text(&p, x, y, imed_table.text[k], fg);
        if (imed_table.comment[k][0]) {
            x += text_w(&p, imed_table.text[k]) + 6;
            painter_text(&p, x, y, imed_table.comment[k], selected ? fg : c[TC_TEXT_DISABLED]);
        }
    }
    if (prev_arrow.w) {
        int ps = imed_table.page_size > 0 ? imed_table.page_size : 5;
        int y = prev_arrow.y + (prev_arrow.h - th) / 2;
        painter_text(&p, prev_arrow.x + 4, y, "‹", first > 0 ? c[TC_TEXT] : c[TC_TEXT_DISABLED]);
        painter_text(&p, next_arrow.x + 2, y, "›", first + ps < imed_table.n ? c[TC_TEXT] : c[TC_TEXT_DISABLED]);
    }
    surface_set_buffer_scale(surface, scale);
    surface_attach(surface, buffer, 0, 0);
    surface_damage(surface, 0, 0, w, h);
    surface_commit(surface);
}

void window_update(int show)
{
    if (show) {
        draw();
    } else if (shown) {
        surface_attach(surface, NULL, 0, 0);
        surface_commit(surface);
    }
    shown = show;
}

int window_click(int x, int y)
{
    if (rect_contains(prev_arrow, x, y)) {
        window_scroll(-1);
        return -1;
    }
    if (rect_contains(next_arrow, x, y)) {
        window_scroll(1);
        return -1;
    }
    for (int i = 0; i < count; i++)
        if (rect_contains(items[i], x, y))
            return first + i;
    return -1;
}

/* window_scroll moves the cursor by whole pages. */
void window_scroll(int pages)
{
    int ps = imed_table.page_size > 0 ? imed_table.page_size : 5;
    int c = (imed_table.cursor / ps + pages) * ps;
    if (c < 0 || c >= imed_table.n)
        return;
    imed_table.cursor = c;
    imed_table_changed();
}
