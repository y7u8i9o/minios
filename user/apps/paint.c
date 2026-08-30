/* paint: draw with the mouse on a canvas. Keys 1 to 7 pick a colour,
 * + and - change the brush size, c clears, Escape quits. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gui/app.h>

static const uint32_t palette[] = {
    0x00000000, 0x00e03030, 0x0030a030, 0x003060e0, 0x00e0c020, 0x00a040c0, 0x00ffffff,
};
static struct app *app;
static struct widget *canvas;
static struct surface img;
static uint32_t color;
static int size = 4, lx, ly;

static void ensure_image(int w, int h)
{
    if (img.width == w && img.height == h)
        return;
    struct surface n = { malloc((size_t)w * h * 4), w, h, w };
    gfx_fill(&n, 0x00ffffff);
    if (img.pixels) {
        gfx_blit(&n, 0, 0, &img, NULL);
        free(img.pixels);
    }
    img = n;
}

static int on_paint(struct widget *w, void *args, void *arg)
{
    ensure_image(w->w, w->h);
    painter_blit(((struct sig_paint *)args)->p, 0, 0, &img);
    return 1;
}

static void dot(int x, int y)
{
    gfx_fill_rect(&img, x - size / 2, y - size / 2, size, size, color);
}

static int on_press(struct widget *w, void *args, void *arg)
{
    struct sig_click *c = args;
    if (!(c->button & 1))
        return 0;
    lx = c->x;
    ly = c->y;
    dot(c->x, c->y);
    widget_invalidate(w);
    return 1;
}

static int on_motion(struct widget *w, void *args, void *arg)
{
    struct sig_click *c = args;
    if (!(c->button & 1))
        return 0;
    int dx = c->x - lx, dy = c->y - ly;
    int steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    if (steps == 0)
        steps = 1;
    for (int i = 1; i <= steps; i++)
        dot(lx + dx * i / steps, ly + dy * i / steps);
    lx = c->x;
    ly = c->y;
    widget_invalidate(w);
    return 1;
}

static int on_key(struct widget *w, void *args, void *arg)
{
    struct sig_key *k = args;
    if (k->ch >= '1' && k->ch <= '7')
        color = palette[k->ch - '1'];
    else if (k->ch == '+' || k->ch == '=')
        size = size < 32 ? size + 2 : size;
    else if (k->ch == '-')
        size = size > 2 ? size - 2 : size;
    else if (k->ch == 'c') {
        gfx_fill(&img, 0x00ffffff);
        widget_invalidate(w);
    } else if (k->code == 0x01)
        app_quit(app, 0);
    return 1;
}

int main(void)
{
    app = app_create();
    if (!app)
        return 1;
    struct widget *win = app_window(app, 480, 360, "paint");
    if (!win)
        return 1;
    widget_set_padding(win, 0);
    canvas = canvas_new(win);
    widget_connect(canvas, "paint", on_paint, NULL);
    widget_connect(canvas, "press", on_press, NULL);
    widget_connect(canvas, "motion", on_motion, NULL);
    widget_connect(canvas, "key", on_key, NULL);
    widget_focus(canvas);
    app_run(app);
    app_destroy(app);
    return 0;
}
