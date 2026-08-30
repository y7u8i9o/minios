/* mandel: the Mandelbrot set in a window. The picture is rendered
 * progressively in passes of shrinking block size, each pass reusing the
 * samples of the previous one, and blocks whose whole boundary shares one
 * iteration count are filled without sampling their interior. Work runs
 * in short slices from a timer so the window stays responsive.
 *
 * Left click or wheel up zooms in at the pointer, right click or wheel
 * down zooms out, dragging pans, arrows pan, + and - zoom around the
 * centre, r resets the view, Escape or q quits. Without a window server
 * (or with `mandel columns rows`) the set is printed as text.
 *
 * Arithmetic is 64 bit fixed point with 26 fraction bits: no floating
 * point is available in user space. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <gui/app.h>

#define FRAC 26
#define ONE (1LL << FRAC)
#define FIRST_BLOCK 32
#define SLICE_MS 30
#define ZOOM_MIN (-4)
#define ZOOM_MAX 16

typedef int64_t fix;

static struct app *app;
static struct widget *canvas, *status;
static struct timer *work_timer;

/* The view: complex coordinate of the canvas centre and units per pixel. */
static fix view_cr, view_ci, view_scale, home_scale;
static int zoom_level, maxiter;

/* The image: iteration counts per pixel (-1 while unknown) and colours. */
static int width, height;
static int32_t *iters;
static struct surface img;
static uint32_t palette[256];

/* Progress of the current render: the sample spacing of the running
 * pass (0 when idle) and the next block row of that pass. */
static int pass, pass_y;
static long render_start;

/* Dragging state: the image is drawn shifted while the button is held. */
static int dragging, drag_moved, drag_x, drag_y, drag_ox, drag_oy;

static fix from_tenths(long t)
{
    return (fix)t * ONE / 10;
}

static int iterate(fix cr, fix ci, int limit)
{
    fix zr = 0, zi = 0;
    for (int i = 0; i < limit; i++) {
        fix zr2 = (zr * zr) >> FRAC, zi2 = (zi * zi) >> FRAC;
        if (zr2 + zi2 > 4 * ONE)
            return i;
        zi = ((zr * zi) >> (FRAC - 1)) + ci;
        zr = zr2 - zi2 + cr;
    }
    return limit;
}

/* ---- text mode ---- */

static int text_mode(int cols, int rows)
{
    const char *shades = " .:-=+*#%@";
    int limit = 40;
    fix rmin = from_tenths(-22), rmax = from_tenths(10);
    fix imin = from_tenths(-12), imax = from_tenths(12);
    if (cols < 2 || rows < 2)
        return 1;
    for (int y = 0; y < rows; y++) {
        fix ci = imin + (imax - imin) * y / (rows - 1);
        for (int x = 0; x < cols; x++) {
            fix cr = rmin + (rmax - rmin) * x / (cols - 1);
            int i = iterate(cr, ci, limit);
            putchar(i == limit ? '@' : shades[i * 9 / limit]);
        }
        putchar('\n');
    }
    return 0;
}

/* ---- colours ---- */

static void palette_init(void)
{
    /* A cycle through five key colours, interpolated linearly. */
    static const uint8_t key[6][3] = {
        { 0, 7, 100 }, { 32, 107, 203 }, { 237, 255, 255 },
        { 255, 170, 0 }, { 0, 2, 0 }, { 0, 7, 100 },
    };
    for (int i = 0; i < 256; i++) {
        int seg = i * 5 / 256, t = i * 5 % 256;
        int c[3];
        for (int k = 0; k < 3; k++)
            c[k] = key[seg][k] + (key[seg + 1][k] - key[seg][k]) * t / 256;
        palette[i] = gfx_rgb(c[0], c[1], c[2]);
    }
}

static uint32_t colour(int it)
{
    return it >= maxiter ? 0x00000000 : palette[(it * 6) & 255];
}

/* ---- the progressive renderer ---- */

static void fill_block(int x, int y, int size, uint32_t c)
{
    int x1 = x + size < width ? x + size : width;
    int y1 = y + size < height ? y + size : height;
    for (int j = y; j < y1; j++)
        for (int i = x; i < x1; i++)
            img.pixels[j * width + i] = c;
}

/* The iteration count at a pixel, computed on first use. A newly
 * computed sample colours the block of the current pass below and to
 * the right of it. */
static int sample(int x, int y)
{
    int32_t *p = &iters[y * width + x];
    if (*p < 0) {
        fix cr = view_cr + (fix)(x - width / 2) * view_scale;
        fix ci = view_ci + (fix)(y - height / 2) * view_scale;
        *p = iterate(cr, ci, maxiter);
        fill_block(x, y, pass, colour(*p));
    }
    return *p;
}

/* Mark a whole block as known: every pixel takes the boundary value, so
 * later passes find nothing left to sample inside it. */
static void solid_block(int x, int y, int size, int it)
{
    int x1 = x + size < width ? x + size : width;
    int y1 = y + size < height ? y + size : height;
    for (int j = y; j < y1; j++)
        for (int i = x; i < x1; i++)
            iters[j * width + i] = it;
    fill_block(x, y, size, colour(it));
}

/* Refine one block of the previous pass: sample the midpoints of its
 * edges, and either fill it when all eight boundary samples agree or
 * sample its centre so the next pass can split it further. */
static void refine_block(int x, int y)
{
    int s = pass, b = 2 * pass;
    int has_r = x + s < width, has_d = y + s < height;
    if (has_r && has_d && iters[(y + s) * width + x + s] >= 0)
        return;                             /* already solid */
    int it = sample(x, y);
    int inside = x + b < width && y + b < height;
    int same = inside;
    if (has_r)
        same &= sample(x + s, y) == it;
    if (has_d)
        same &= sample(x, y + s) == it;
    if (x + b < width) {
        same &= sample(x + b, y) == it;
        if (has_d)
            same &= sample(x + b, y + s) == it;
    }
    if (y + b < height) {
        same &= sample(x, y + b) == it;
        if (has_r)
            same &= sample(x + s, y + b) == it;
    }
    if (inside)
        same &= sample(x + b, y + b) == it;
    if (same)
        solid_block(x, y, b, it);
    else if (has_r && has_d)
        sample(x + s, y + s);
}

/* One block row of the current pass. Returns 0 when the render is done. */
static int render_row(void)
{
    if (pass == 0)
        return 0;
    if (pass == FIRST_BLOCK) {
        for (int x = 0; x < width; x += pass)
            sample(x, pass_y);
        pass_y += pass;
    } else {
        for (int x = 0; x < width; x += 2 * pass)
            refine_block(x, pass_y);
        pass_y += 2 * pass;
    }
    if (pass_y >= height) {
        pass /= 2;
        pass_y = 0;
    }
    return pass != 0;
}

static void status_update(void)
{
    char buf[128];
    long re = (long)(view_cr * 10000 / ONE), im = (long)(view_ci * 10000 / ONE);
    long zoom = zoom_level >= 0 ? 1L << zoom_level : 0;
    if (zoom)
        snprintf(buf, sizeof buf, "centre %ld.%04ld %c%ld.%04ldi  zoom %ldx  %d iterations  %s",
                 re / 10000, labs(re % 10000), im < 0 ? '-' : '+', labs(im) / 10000, labs(im) % 10000,
                 zoom, maxiter, pass ? "rendering" : "done");
    else
        snprintf(buf, sizeof buf, "centre %ld.%04ld %c%ld.%04ldi  zoom 1/%ldx  %d iterations  %s",
                 re / 10000, labs(re % 10000), im < 0 ? '-' : '+', labs(im) / 10000, labs(im) % 10000,
                 1L << -zoom_level, maxiter, pass ? "rendering" : "done");
    widget_set_text(status, buf);
}

static void work(void *arg)
{
    long deadline = uptime_ms() + SLICE_MS;
    int more = 1;
    while (more && uptime_ms() < deadline)
        more = render_row();
    widget_invalidate(canvas);
    if (!more) {
        app_timer_remove(app, work_timer);
        work_timer = NULL;
        printf("mandel: render complete in %ld ms\n", uptime_ms() - render_start);
        fflush(stdout);
        status_update();
    }
}

static void render_start_view(void)
{
    if (!iters)
        return;
    for (int i = 0; i < width * height; i++)
        iters[i] = -1;
    maxiter = 64 + 24 * (zoom_level > 0 ? zoom_level : 0);
    pass = FIRST_BLOCK;
    pass_y = 0;
    render_start = uptime_ms();
    printf("mandel: rendering %dx%d at zoom level %d\n", width, height, zoom_level);
    fflush(stdout);
    if (!work_timer)
        work_timer = app_timer_add(app, 1, 1, work, NULL);
    status_update();
}

static int resize_image(int w, int h)
{
    if (w == width && h == height)
        return 0;
    free(iters);
    free(img.pixels);
    iters = malloc(sizeof *iters * (size_t)w * h);
    img.pixels = malloc(sizeof *img.pixels * (size_t)w * h);
    if (!iters || !img.pixels) {
        free(iters);
        free(img.pixels);
        iters = NULL;
        img.pixels = NULL;
        width = height = 0;
        return -1;
    }
    width = img.width = img.stride = w;
    height = img.height = h;
    memset(img.pixels, 0, sizeof *img.pixels * (size_t)w * h);
    return 1;
}

/* ---- view changes ---- */

static void view_reset(void)
{
    view_cr = from_tenths(-6);
    view_ci = 0;
    zoom_level = 0;
    view_scale = home_scale;
}

static void scale_for_level(void)
{
    view_scale = zoom_level >= 0 ? home_scale >> zoom_level : home_scale << -zoom_level;
}

/* Zoom by one level keeping the complex number under pixel (x, y) fixed. */
static void zoom_at(int x, int y, int dir)
{
    int level = zoom_level + dir;
    if (level < ZOOM_MIN || level > ZOOM_MAX)
        return;
    fix cr = view_cr + (fix)(x - width / 2) * view_scale;
    fix ci = view_ci + (fix)(y - height / 2) * view_scale;
    zoom_level = level;
    scale_for_level();
    view_cr = cr - (fix)(x - width / 2) * view_scale;
    view_ci = ci - (fix)(y - height / 2) * view_scale;
    render_start_view();
}

/* Move the view by a pixel offset; the old picture is shifted so the
 * known part stays in place while the rest is rendered. */
static void pan(int dx, int dy)
{
    view_cr += (fix)dx * view_scale;
    view_ci += (fix)dy * view_scale;
    if (iters) {
        for (int j = 0; j < height; j++) {
            int sj = dy >= 0 ? j : height - 1 - j;
            int from = sj + dy;
            uint32_t *row = img.pixels + (size_t)sj * width;
            if (from < 0 || from >= height) {
                memset(row, 0, sizeof *row * (size_t)width);
                continue;
            }
            const uint32_t *src = img.pixels + (size_t)from * width;
            int n = width - abs(dx);
            if (n <= 0) {
                memset(row, 0, sizeof *row * (size_t)width);
                continue;
            }
            memmove(row + (dx < 0 ? -dx : 0), src + (dx > 0 ? dx : 0), sizeof *row * (size_t)n);
            if (dx > 0)
                memset(row + n, 0, sizeof *row * (size_t)dx);
            else if (dx < 0)
                memset(row, 0, sizeof *row * (size_t)-dx);
        }
    }
    render_start_view();
}

/* ---- signal handlers ---- */

static int on_paint(struct widget *w, void *args, void *arg)
{
    struct painter *p = ((struct sig_paint *)args)->p;
    int r = resize_image(w->w, w->h);
    if (r < 0) {
        painter_fill(p, 0, 0, w->w, w->h, 0x00000000);
        return 1;
    }
    if (r > 0)
        render_start_view();
    if (drag_ox || drag_oy)
        painter_fill(p, 0, 0, w->w, w->h, 0x00000000);
    painter_blit(p, drag_ox, drag_oy, &img);
    return 1;
}

static int on_press(struct widget *w, void *args, void *arg)
{
    struct sig_click *c = args;
    widget_focus(w);
    if (c->button & 1) {
        dragging = 1;
        drag_moved = 0;
        drag_x = c->x;
        drag_y = c->y;
    } else if (c->button & 2) {
        zoom_at(c->x, c->y, -1);
    }
    return 1;
}

static int on_motion(struct widget *w, void *args, void *arg)
{
    struct sig_click *c = args;
    if (!dragging)
        return 0;
    drag_ox = c->x - drag_x;
    drag_oy = c->y - drag_y;
    if (abs(drag_ox) > 3 || abs(drag_oy) > 3)
        drag_moved = 1;
    widget_invalidate(w);
    return 1;
}

static int on_release(struct widget *w, void *args, void *arg)
{
    struct sig_click *c = args;
    if (!dragging)
        return 0;
    dragging = 0;
    int ox = drag_ox, oy = drag_oy;
    drag_ox = drag_oy = 0;
    if (drag_moved)
        pan(-ox, -oy);
    else
        zoom_at(c->x, c->y, 1);
    widget_invalidate(w);
    return 1;
}

static int on_wheel(struct widget *w, void *args, void *arg)
{
    struct sig_click *c = args;
    zoom_at(c->x, c->y, c->button < 0 ? 1 : -1);
    return 1;
}

static int on_key(struct widget *w, void *args, void *arg)
{
    struct sig_key *k = args;
    switch (k->code) {
    case 0x01: app_quit(app, 0); return 1;              /* escape */
    case 0x48: pan(0, -height / 4); return 1;           /* up */
    case 0x50: pan(0, height / 4); return 1;            /* down */
    case 0x4b: pan(-width / 4, 0); return 1;            /* left */
    case 0x4d: pan(width / 4, 0); return 1;             /* right */
    }
    switch (k->ch) {
    case 'q': app_quit(app, 0); return 1;
    case '+': case '=': zoom_at(width / 2, height / 2, 1); return 1;
    case '-': zoom_at(width / 2, height / 2, -1); return 1;
    case 'r':
        view_reset();
        render_start_view();
        return 1;
    }
    return 0;
}

static int on_zoom_in(struct widget *w, void *args, void *arg)
{
    zoom_at(width / 2, height / 2, 1);
    return 1;
}

static int on_zoom_out(struct widget *w, void *args, void *arg)
{
    zoom_at(width / 2, height / 2, -1);
    return 1;
}

static int on_reset(struct widget *w, void *args, void *arg)
{
    view_reset();
    render_start_view();
    return 1;
}

int main(int argc, char **argv)
{
    if (argc > 2)
        return text_mode(atoi(argv[1]), atoi(argv[2]));
    app = app_create();
    if (!app) {
        int cols = 78, rows = 22;
        struct winsize ws;
        if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 10) {
            cols = ws.ws_col - 2;
            rows = ws.ws_row - 2;
        }
        return text_mode(cols, rows);
    }
    palette_init();
    /* The home view spans 3.2 units of the real axis over 640 pixels. */
    home_scale = from_tenths(32) / 640;
    view_reset();

    struct widget *win = app_window(app, 640, 480, "mandel");
    if (!win)
        return 1;
    struct widget *bar = box_new(win, 0);
    widget_set_stretch(bar, 1, 0);
    struct widget *b = button_new(bar, "Zoom &in");
    widget_connect(b, "clicked", on_zoom_in, NULL);
    b = button_new(bar, "Zoom &out");
    widget_connect(b, "clicked", on_zoom_out, NULL);
    b = button_new(bar, "&Reset");
    widget_connect(b, "clicked", on_reset, NULL);
    status = label_new(bar, "");
    widget_set_stretch(status, 1, 0);
    canvas = canvas_new(win);
    widget_connect(canvas, "paint", on_paint, NULL);
    widget_connect(canvas, "press", on_press, NULL);
    widget_connect(canvas, "motion", on_motion, NULL);
    widget_connect(canvas, "release", on_release, NULL);
    widget_connect(canvas, "wheel", on_wheel, NULL);
    widget_connect(canvas, "key", on_key, NULL);
    widget_focus(canvas);
    status_update();

    int code = app_run(app);
    app_destroy(app);
    printf("mandel: exit %d\n", code);
    return code;
}
