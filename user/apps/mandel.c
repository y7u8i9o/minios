/* mandel: the Mandelbrot set and its Julia sets in a window.
 *
 * The picture is divided into tiles of TILE pixels, which worker threads
 * take in the order of their distance from the centre of the window.  A
 * tile is rendered progressively in passes of shrinking block size, each
 * pass reusing the samples of the previous one.  A block whose boundary
 * samples are all inside the set is filled without sampling its interior.
 * The main thread draws the image every REDRAW_MS while a render runs.
 *
 * The arithmetic is double precision.  Escaping points are coloured with
 * a smooth iteration count, mu = n + 1 - log2(log |z|), interpolated in
 * one of four palettes.  The magnification ranges from 1/16 to 2^44.
 *
 * A left click or wheel up zooms in at the pointer, wheel down zooms out,
 * a left drag pans, a right drag zooms into the rectangle and a right
 * click zooms out.  The arrow keys pan, + and - zoom around the centre,
 * r resets the view, j switches between the Mandelbrot set and the Julia
 * set of the centre, and Escape or q quits.  Without a window server, or
 * with `mandel columns rows`, the set is printed as text. */
#include <minios/conf.h>
#include <langinfo.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <gui/app.h>
#include <gui/image.h>
#include <gui/i18n.h>

#define TILE 64
#define FIRST_BLOCK 16
#define REDRAW_MS 40
#define MAX_WORKERS 8
#define LEVEL_MIN (-4)
#define LEVEL_MAX 44
#define BAILOUT 65536.0                 /* The square of the escape radius 256. */

/* The parameters of one render, copied by each worker when it takes a
 * tile. */
struct view {
    double cr, ci, scale;               /* The centre and the units per pixel. */
    int julia;
    double jr, ji;                      /* The constant of the Julia set. */
    int maxiter, width, height;
    int palette;
};

static struct app *app;
static struct widget *win, *canvas, *st_centre, *st_pointer, *st_zoom, *st_state;
static struct widget *palette_box, *iter_box, *mandel_item, *julia_item;
static struct timer *redraw_timer;

/* The view of the window, changed by the main thread only. */
static struct view view;
static double home_scale;
static struct view saved_mandel;        /* The Mandelbrot view while a Julia set is shown. */
static int iter_choice;                 /* 0 selects the iteration count automatically. */
static int palette_index;              /* The palette of the next render and of recolour. */

/* The image.  iters is -1 for a pixel that is not computed yet, smooth is
 * the smooth iteration count of a computed pixel.  Each tile is written
 * by one worker only.  The main thread reads pixels while workers write
 * them, and the workers store each pixel with one relaxed atomic store. */
static int width, height;
static int32_t *iters;
static float *smooth;
static struct surface img;
static uint32_t palettes[4][256];

/* The render state.  lock protects job, running, active, next_tile,
 * ntiles and tiles_done.  cancel is read by the workers without the lock
 * with relaxed atomic loads. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER, idle = PTHREAD_COND_INITIALIZER;
static struct view job;
static int running, active, next_tile, ntiles, tiles_done;
static int cancel;
static int *tile_order;
static int nworkers;
static long render_start, render_ms = -1;

/* Dragging state: a left drag shifts the drawn image, a right drag draws
 * the zoom rectangle. */
static int dragging, drag_button, drag_moved, drag_x, drag_y, drag_ox, drag_oy;

static int level_of(const struct view *v)
{
    return (int)floor(log2(home_scale / v->scale) + 0.5);
}

/* iterate returns the smooth iteration count of the orbit of z0 under
 * z = z^2 + c, or limit when the orbit stays bounded for limit steps. */
static double iterate(double zr, double zi, double cr, double ci, int limit, int julia)
{
    if (!julia) {
        /* The main cardioid and the period 2 bulb are inside the set. */
        double xq = cr - 0.25, q = xq * xq + ci * ci;
        if (q * (q + xq) <= 0.25 * ci * ci || (cr + 1) * (cr + 1) + ci * ci <= 0.0625)
            return limit;
    }
    for (int i = 0; i < limit; i++) {
        double zr2 = zr * zr, zi2 = zi * zi;
        if (zr2 + zi2 > BAILOUT)
            return i + 1 - log2(0.5 * log(zr2 + zi2));
        zi = 2 * zr * zi + ci;
        zr = zr2 - zi2 + cr;
    }
    return limit;
}

/* The functions below print the set as text. */

static int text_mode(int cols, int rows)
{
    const char *shades = " .:-=+*#%@";
    int limit = 40;
    if (cols < 2 || rows < 2)
        return 1;
    for (int y = 0; y < rows; y++) {
        double ci = -1.2 + 2.4 * y / (rows - 1);
        for (int x = 0; x < cols; x++) {
            double cr = -2.2 + 3.2 * x / (cols - 1);
            double mu = iterate(0, 0, cr, ci, limit, 0);
            putchar(mu >= limit ? '@' : shades[(int)mu * 9 / limit]);
        }
        putchar('\n');
    }
    return 0;
}

/* The functions below compute the colours. */

static void palette_from_keys(uint32_t *out, const uint8_t (*key)[3], int nkeys)
{
    for (int i = 0; i < 256; i++) {
        int seg = i * (nkeys - 1) / 256, t = i * (nkeys - 1) % 256;
        int c[3];
        for (int k = 0; k < 3; k++)
            c[k] = key[seg][k] + (key[seg + 1][k] - key[seg][k]) * t / 256;
        out[i] = gfx_rgb(c[0], c[1], c[2]);
    }
}

static void palettes_init(void)
{
    static const uint8_t classic[6][3] = {
        { 0, 7, 100 }, { 32, 107, 203 }, { 237, 255, 255 }, { 255, 170, 0 }, { 0, 2, 0 }, { 0, 7, 100 },
    };
    static const uint8_t fire[5][3] = {
        { 20, 0, 0 }, { 180, 30, 0 }, { 255, 200, 40 }, { 255, 255, 220 }, { 20, 0, 0 },
    };
    static const uint8_t ocean[5][3] = {
        { 0, 20, 40 }, { 0, 110, 140 }, { 120, 220, 210 }, { 240, 250, 255 }, { 0, 20, 40 },
    };
    static const uint8_t grey[3][3] = { { 16, 16, 16 }, { 240, 240, 240 }, { 16, 16, 16 } };
    palette_from_keys(palettes[0], classic, 6);
    palette_from_keys(palettes[1], fire, 5);
    palette_from_keys(palettes[2], ocean, 5);
    palette_from_keys(palettes[3], grey, 3);
}

static uint32_t mix(uint32_t a, uint32_t b, int t)
{
    uint32_t r = (((a >> 16) & 255) * (uint32_t)(256 - t) + ((b >> 16) & 255) * (uint32_t)t) >> 8;
    uint32_t g = (((a >> 8) & 255) * (uint32_t)(256 - t) + ((b >> 8) & 255) * (uint32_t)t) >> 8;
    uint32_t bl = ((a & 255) * (uint32_t)(256 - t) + (b & 255) * (uint32_t)t) >> 8;
    return r << 16 | g << 8 | bl;
}

/* colour maps a smooth iteration count to a colour.  The palette repeats
 * every 256 / 6 iterations, and the points of the set are black. */
static uint32_t colour(double mu, int maxiter, const uint32_t *pal)
{
    if (mu >= maxiter)
        return 0;
    double f = mu * 6;
    if (f < 0)
        f = 0;
    long i = (long)f;
    int t = (int)((f - (double)i) * 256);
    return mix(pal[i & 255], pal[(i + 1) & 255], t);
}

/* The functions below render one tile.  They run in the worker threads. */

struct tile {
    int x0, y0, x1, y1;
    int block;                          /* The block size of the running pass. */
    const struct view *v;
    const uint32_t *pal;
};

static void put_pixel(int x, int y, uint32_t c)
{
    __atomic_store_n(&img.pixels[y * width + x], c, __ATOMIC_RELAXED);
}

static void fill_block(struct tile *t, int x, int y, int size, uint32_t c)
{
    int x1 = x + size < t->x1 ? x + size : t->x1;
    int y1 = y + size < t->y1 ? y + size : t->y1;
    for (int j = y; j < y1; j++)
        for (int i = x; i < x1; i++)
            put_pixel(i, j, c);
}

/* sample returns the iteration count at a pixel and computes it on first
 * use.  A new sample colours the block of the current pass below and to
 * the right of it. */
static int sample(struct tile *t, int x, int y)
{
    int32_t *p = &iters[y * width + x];
    if (*p < 0) {
        const struct view *v = t->v;
        double pr = v->cr + (x - v->width / 2) * v->scale;
        double pi = v->ci + (y - v->height / 2) * v->scale;
        double mu = v->julia ? iterate(pr, pi, v->jr, v->ji, v->maxiter, 1) : iterate(0, 0, pr, pi, v->maxiter, 0);
        *p = mu >= v->maxiter ? v->maxiter : (int)mu;
        smooth[y * width + x] = (float)mu;
        fill_block(t, x, y, t->block, colour(mu, v->maxiter, t->pal));
    }
    return *p;
}

/* solid_block marks a block as inside the set. */
static void solid_block(struct tile *t, int x, int y, int size)
{
    int x1 = x + size < t->x1 ? x + size : t->x1;
    int y1 = y + size < t->y1 ? y + size : t->y1;
    for (int j = y; j < y1; j++)
        for (int i = x; i < x1; i++) {
            iters[j * width + i] = t->v->maxiter;
            smooth[j * width + i] = (float)t->v->maxiter;
            put_pixel(i, j, 0);
        }
}

/* refine_block refines one block of the previous pass.  It samples the
 * midpoints of the edges and fills the block when all eight boundary
 * samples are inside the set.  Otherwise it samples the centre, and the
 * next pass splits the block further.  Samples outside the tile are not
 * taken, because another worker computes them. */
static void refine_block(struct tile *t, int x, int y)
{
    int s = t->block, b = 2 * s, maxiter = t->v->maxiter;
    int has_r = x + s < t->x1, has_d = y + s < t->y1;
    if (has_r && has_d && iters[(y + s) * width + x + s] >= 0)
        return;                         /* The block is already solid. */
    int inside = x + b < t->x1 && y + b < t->y1;
    int same = inside && sample(t, x, y) == maxiter;
    if (has_r)
        same &= sample(t, x + s, y) == maxiter;
    if (has_d)
        same &= sample(t, x, y + s) == maxiter;
    if (x + b < t->x1) {
        same &= sample(t, x + b, y) == maxiter;
        if (has_d)
            same &= sample(t, x + b, y + s) == maxiter;
    }
    if (y + b < t->y1) {
        same &= sample(t, x, y + b) == maxiter;
        if (has_r)
            same &= sample(t, x + s, y + b) == maxiter;
    }
    if (inside)
        same &= sample(t, x + b, y + b) == maxiter;
    if (same)
        solid_block(t, x, y, b);
    else if (has_r && has_d)
        sample(t, x + s, y + s);
}

static int cancelled(void)
{
    return __atomic_load_n(&cancel, __ATOMIC_RELAXED);
}

static void render_tile(int index, const struct view *v)
{
    int cols = (v->width + TILE - 1) / TILE;
    struct tile t = { (index % cols) * TILE, (index / cols) * TILE, 0, 0, FIRST_BLOCK, v, palettes[v->palette] };
    t.x1 = t.x0 + TILE < v->width ? t.x0 + TILE : v->width;
    t.y1 = t.y0 + TILE < v->height ? t.y0 + TILE : v->height;
    for (int y = t.y0; y < t.y1 && !cancelled(); y += FIRST_BLOCK)
        for (int x = t.x0; x < t.x1; x += FIRST_BLOCK)
            sample(&t, x, y);
    for (t.block = FIRST_BLOCK / 2; t.block >= 1; t.block /= 2)
        for (int y = t.y0; y < t.y1; y += 2 * t.block) {
            if (cancelled())
                return;
            for (int x = t.x0; x < t.x1; x += 2 * t.block)
                refine_block(&t, x, y);
        }
}

static void *worker(void *arg)
{
    for (;;) {
        pthread_mutex_lock(&lock);
        while (!running || next_tile >= ntiles)
            pthread_cond_wait(&wake, &lock);
        int index = tile_order[next_tile++];
        struct view v = job;
        active++;
        pthread_mutex_unlock(&lock);
        render_tile(index, &v);
        pthread_mutex_lock(&lock);
        active--;
        if (!cancelled())
            tiles_done++;
        if (active == 0)
            pthread_cond_signal(&idle);
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}

/* The functions below control the render from the main thread. */

/* render_stop cancels the running render and waits until no worker
 * writes into the image. */
static void render_stop(void)
{
    pthread_mutex_lock(&lock);
    __atomic_store_n(&cancel, 1, __ATOMIC_RELAXED);
    running = 0;
    while (active > 0)
        pthread_cond_wait(&idle, &lock);
    __atomic_store_n(&cancel, 0, __ATOMIC_RELAXED);
    pthread_mutex_unlock(&lock);
}

static int auto_iterations(const struct view *v)
{
    int level = level_of(v);
    return 160 + 64 * (level > 0 ? level : 0);
}

static int tile_distance(int index, int cols)
{
    int dx = (index % cols) * TILE + TILE / 2 - width / 2, dy = (index / cols) * TILE + TILE / 2 - height / 2;
    return dx * dx + dy * dy;
}

static int cols_for_sort;
static int compare_tiles(const void *a, const void *b)
{
    return tile_distance(*(const int *)a, cols_for_sort) - tile_distance(*(const int *)b, cols_for_sort);
}

static void status_update(void);
static void redraw(void *arg);

static void render_start_view(void)
{
    if (!iters)
        return;
    render_stop();
    for (int i = 0; i < width * height; i++)
        iters[i] = -1;
    static const int fixed[] = { 0, 256, 1024, 4096 };
    view.maxiter = iter_choice ? fixed[iter_choice] : auto_iterations(&view);
    view.width = width;
    view.height = height;
    view.palette = palette_index;
    int cols = (width + TILE - 1) / TILE, rows = (height + TILE - 1) / TILE;
    pthread_mutex_lock(&lock);
    job = view;
    ntiles = cols * rows;
    free(tile_order);
    tile_order = malloc(sizeof *tile_order * (size_t)ntiles);
    for (int i = 0; i < ntiles; i++)
        tile_order[i] = i;
    cols_for_sort = cols;
    qsort(tile_order, (size_t)ntiles, sizeof *tile_order, compare_tiles);
    next_tile = tiles_done = 0;
    running = 1;
    pthread_cond_broadcast(&wake);
    pthread_mutex_unlock(&lock);
    render_start = uptime_ms();
    render_ms = -1;
    printf("mandel: rendering %dx%d at zoom level %d\n", width, height, level_of(&view));
    fflush(stdout);
    if (!redraw_timer)
        redraw_timer = app_timer_add(app, REDRAW_MS, 1, redraw, NULL);
    status_update();
}

/* recolour paints a finished image again with the current palette. */
static void recolour(void)
{
    const uint32_t *pal = palettes[palette_index];
    for (int i = 0; i < width * height; i++)
        img.pixels[i] = colour(smooth[i], view.maxiter, pal);
    widget_invalidate(canvas);
}

static void redraw(void *arg)
{
    pthread_mutex_lock(&lock);
    int done = running && tiles_done == ntiles && active == 0;
    if (done)
        running = 0;
    pthread_mutex_unlock(&lock);
    widget_invalidate(canvas);
    if (done) {
        app_timer_remove(app, redraw_timer);
        redraw_timer = NULL;
        render_ms = uptime_ms() - render_start;
        printf("mandel: render complete in %ld ms\n", render_ms);
        fflush(stdout);
    }
    status_update();
}

static int resize_image(int w, int h)
{
    if (w == width && h == height)
        return 0;
    render_stop();
    free(iters);
    free(smooth);
    free(img.pixels);
    iters = malloc(sizeof *iters * (size_t)w * h);
    smooth = malloc(sizeof *smooth * (size_t)w * h);
    img.pixels = malloc(sizeof *img.pixels * (size_t)w * h);
    if (!iters || !smooth || !img.pixels) {
        free(iters);
        free(smooth);
        free(img.pixels);
        iters = NULL;
        smooth = NULL;
        img.pixels = NULL;
        width = height = 0;
        return -1;
    }
    width = img.width = img.stride = w;
    height = img.height = h;
    memset(img.pixels, 0, sizeof *img.pixels * (size_t)w * h);
    return 1;
}

/* The functions below change the view. */

static void view_reset(void)
{
    view.cr = view.julia ? 0 : -0.6;
    view.ci = 0;
    view.scale = home_scale;
}

static void set_scale(double scale)
{
    double lo = home_scale / ldexp(1.0, LEVEL_MAX), hi = home_scale * ldexp(1.0, -LEVEL_MIN);
    view.scale = scale < lo ? lo : scale > hi ? hi : scale;
}

/* zoom_at divides the scale by factor and leaves the point under pixel
 * (x, y) in place. */
static void zoom_at(int x, int y, double factor)
{
    double pr = view.cr + (x - width / 2) * view.scale, pi = view.ci + (y - height / 2) * view.scale;
    set_scale(view.scale / factor);
    view.cr = pr - (x - width / 2) * view.scale;
    view.ci = pi - (y - height / 2) * view.scale;
    render_start_view();
}

/* zoom_rect shows the rectangle between two pixels in the whole canvas. */
static void zoom_rect(int x0, int y0, int x1, int y1)
{
    int rw = abs(x1 - x0), rh = abs(y1 - y0);
    if (rw < 4 || rh < 4)
        return;
    double mr = view.cr + ((x0 + x1) / 2.0 - width / 2) * view.scale;
    double mi = view.ci + ((y0 + y1) / 2.0 - height / 2) * view.scale;
    double fx = (double)rw / width, fy = (double)rh / height;
    set_scale(view.scale * (fx > fy ? fx : fy));
    view.cr = mr;
    view.ci = mi;
    render_start_view();
}

/* pan moves the view by a pixel offset.  The old picture is shifted so
 * that the known part stays in place while the rest is rendered. */
static void pan(int dx, int dy)
{
    render_stop();
    view.cr += dx * view.scale;
    view.ci += dy * view.scale;
    if (iters) {
        for (int j = 0; j < height; j++) {
            int sj = dy >= 0 ? j : height - 1 - j;
            int from = sj + dy;
            uint32_t *row = img.pixels + (size_t)sj * width;
            int n = width - abs(dx);
            if (from < 0 || from >= height || n <= 0) {
                memset(row, 0, sizeof *row * (size_t)width);
                continue;
            }
            const uint32_t *src = img.pixels + (size_t)from * width;
            memmove(row + (dx < 0 ? -dx : 0), src + (dx > 0 ? dx : 0), sizeof *row * (size_t)n);
            if (dx > 0)
                memset(row + n, 0, sizeof *row * (size_t)dx);
            else if (dx < 0)
                memset(row, 0, sizeof *row * (size_t)-dx);
        }
    }
    render_start_view();
}

static void set_julia(int on)
{
    if (on == view.julia)
        return;
    if (on) {
        saved_mandel = view;
        view.jr = view.cr;
        view.ji = view.ci;
        view.julia = 1;
        view_reset();
    } else {
        view = saved_mandel;
        view.julia = 0;
    }
    widget_set_enabled(mandel_item, view.julia);
    widget_set_enabled(julia_item, !view.julia);
    printf("mandel: %s\n", view.julia ? "Julia set" : "Mandelbrot set");
    fflush(stdout);
    render_start_view();
}

/* The functions below format the status bar. */

static int coordinate_digits(void)
{
    int d = 3 + (int)ceil(log10(home_scale / view.scale + 1));
    return d > 16 ? 16 : d;
}

static void format_complex(char *buf, size_t size, double re, double im)
{
    int d = coordinate_digits();
    snprintf(buf, size, "%.*f %c %.*fi", d, re, im < 0 ? '-' : '+', d, fabs(im));
}

static void status_update(void)
{
    char buf[128], c[96];
    format_complex(c, sizeof c, view.cr, view.ci);
    if (view.julia) {
        char jc[96];
        format_complex(jc, sizeof jc, view.jr, view.ji);
        snprintf(buf, sizeof buf, _("Julia %s"), jc);
    } else {
        snprintf(buf, sizeof buf, _("Centre %s"), c);
    }
    widget_set_text(st_centre, buf);
    double mag = home_scale / view.scale;
    if (mag >= 1e6)
        snprintf(buf, sizeof buf, _("Zoom %.2e"), mag);
    else if (mag >= 1)
        snprintf(buf, sizeof buf, _("Zoom %.0fx"), mag);
    else
        snprintf(buf, sizeof buf, _("Zoom 1/%.0fx"), 1 / mag);
    widget_set_text(st_zoom, buf);
    pthread_mutex_lock(&lock);
    int done = tiles_done, total = ntiles, run = running;
    pthread_mutex_unlock(&lock);
    if (run)
        snprintf(buf, sizeof buf, ngettext("%d iteration, %d%%", "%d iterations, %d%%", (unsigned long)view.maxiter),
                 view.maxiter, total ? done * 100 / total : 0);
    else if (render_ms >= 1000)
        snprintf(buf, sizeof buf, ngettext("%d iteration, %ld%s%01ld s", "%d iterations, %ld%s%01ld s",
                                           (unsigned long)view.maxiter),
                 view.maxiter, render_ms / 1000, nl_langinfo(RADIXCHAR), render_ms % 1000 / 100);
    else if (render_ms >= 0)
        snprintf(buf, sizeof buf, ngettext("%d iteration, %ld ms", "%d iterations, %ld ms", (unsigned long)view.maxiter),
                 view.maxiter, render_ms);
    else
        snprintf(buf, sizeof buf, ngettext("%d iteration", "%d iterations", (unsigned long)view.maxiter), view.maxiter);
    widget_set_text(st_state, buf);
}

/* The functions below process input events. */

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
    int ox = dragging && drag_button == 1 ? drag_ox : 0, oy = dragging && drag_button == 1 ? drag_oy : 0;
    if (ox || oy)
        painter_fill(p, 0, 0, w->w, w->h, 0x00000000);
    painter_blit(p, ox, oy, &img);
    if (dragging && drag_button == 2 && drag_moved) {
        int x0 = drag_x < drag_x + drag_ox ? drag_x : drag_x + drag_ox;
        int y0 = drag_y < drag_y + drag_oy ? drag_y : drag_y + drag_oy;
        painter_frame(p, x0, y0, abs(drag_ox), abs(drag_oy), 0x00ffffff);
        painter_frame(p, x0 - 1, y0 - 1, abs(drag_ox) + 2, abs(drag_oy) + 2, 0x00000000);
    }
    return 1;
}

static int on_press(struct widget *w, void *args, void *arg)
{
    struct sig_click *c = args;
    widget_focus(w);
    if (c->button & 3) {
        dragging = 1;
        drag_button = c->button & 1 ? 1 : 2;
        drag_moved = 0;
        drag_x = c->x;
        drag_y = c->y;
        drag_ox = drag_oy = 0;
    }
    return 1;
}

static int on_motion(struct widget *w, void *args, void *arg)
{
    struct sig_click *c = args;
    char pos[96], text[112];
    format_complex(pos, sizeof pos, view.cr + (c->x - width / 2) * view.scale, view.ci + (c->y - height / 2) * view.scale);
    snprintf(text, sizeof text, _("Pointer %s"), pos);
    widget_set_text(st_pointer, text);
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
    if (drag_button == 1)
        drag_moved ? pan(-ox, -oy) : zoom_at(c->x, c->y, 2);
    else
        drag_moved ? zoom_rect(drag_x, drag_y, drag_x + ox, drag_y + oy) : zoom_at(c->x, c->y, 0.5);
    widget_invalidate(w);
    return 1;
}

static int on_wheel(struct widget *w, void *args, void *arg)
{
    struct sig_click *c = args;
    zoom_at(c->x, c->y, c->button < 0 ? 2 : 0.5);
    return 1;
}

static int on_key(struct widget *w, void *args, void *arg)
{
    struct sig_key *k = args;
    switch (k->code) {
    case KEY_ESC: app_quit(app, 0); return 1;
    case KEY_UP: pan(0, -height / 4); return 1;
    case KEY_DOWN: pan(0, height / 4); return 1;
    case KEY_LEFT: pan(-width / 4, 0); return 1;
    case KEY_RIGHT: pan(width / 4, 0); return 1;
    }
    if (k->mods & (WMOD_CTRL | WMOD_ALT))
        return 0;
    switch (k->ch) {
    case 'q': app_quit(app, 0); return 1;
    case '+': case '=': zoom_at(width / 2, height / 2, 2); return 1;
    case '-': zoom_at(width / 2, height / 2, 0.5); return 1;
    case 'j': set_julia(!view.julia); return 1;
    case 'r':
        view_reset();
        render_start_view();
        return 1;
    }
    return 0;
}

static int on_zoom_in(struct widget *w, void *args, void *arg) { zoom_at(width / 2, height / 2, 2); return 1; }
static int on_zoom_out(struct widget *w, void *args, void *arg) { zoom_at(width / 2, height / 2, 0.5); return 1; }
static int on_mandel(struct widget *w, void *args, void *arg) { set_julia(0); return 1; }
static int on_julia(struct widget *w, void *args, void *arg) { set_julia(1); return 1; }

static int on_reset(struct widget *w, void *args, void *arg)
{
    view_reset();
    render_start_view();
    return 1;
}

static int on_palette(struct widget *w, void *args, void *arg)
{
    palette_index = w->value;
    pthread_mutex_lock(&lock);
    int run = running;
    pthread_mutex_unlock(&lock);
    if (run)
        render_start_view();
    else
        recolour();
    widget_focus(canvas);
    return 1;
}

static int on_iterations(struct widget *w, void *args, void *arg)
{
    iter_choice = w->value;
    render_start_view();
    widget_focus(canvas);
    return 1;
}

static int on_save(struct widget *w, void *args, void *arg)
{
    static char name[256];
    const char *const buttons[] = { _("Close") };
    if (!name[0])
        snprintf(name, sizeof name, "%s/mandel.png", conf_home());
    const struct file_filter filters[] = { { _("PNG images"), "*.png" }, { _("All files"), "*" } };
    if (!app_choose_file(app, FILE_CHOOSER_SAVE, _("Save image"), filters, 2, name, sizeof name))
        return 1;
    render_stop();
    struct image *out = image_create(width, height);
    int ok = out != NULL;
    if (out) {
        for (int i = 0; i < width * height; i++)
            out->pixels[i] = 0xff000000u | img.pixels[i];
        ok = image_save_png(out, name) == 0;
        image_free(out);
    }
    if (!ok)
        app_dialog(app, _("Error"), _("The image cannot be written."), buttons, 1);
    else {
        printf("mandel: saved %s\n", name);
        fflush(stdout);
    }
    pthread_mutex_lock(&lock);
    int unfinished = tiles_done < ntiles;
    pthread_mutex_unlock(&lock);
    if (unfinished)
        render_start_view();
    return 1;
}

static int on_quit(struct widget *w, void *args, void *arg)
{
    app_quit(app, 0);
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
    textdomain("mandel");
    palettes_init();
    /* The home view spans 3.2 units of the real axis over 640 pixels. */
    home_scale = 3.2 / 640;
    view_reset();
    nworkers = nproc();
    if (nworkers < 1)
        nworkers = 1;
    if (nworkers > MAX_WORKERS)
        nworkers = MAX_WORKERS;
    for (int i = 0; i < nworkers; i++) {
        pthread_t t;
        if (pthread_create(&t, NULL, worker, NULL) != 0) {
            nworkers = i;
            break;
        }
    }
    if (nworkers == 0) {
        fprintf(stderr, "mandel: cannot create a worker thread\n");
        return 1;
    }
    printf("mandel: %d worker threads\n", nworkers);
    fflush(stdout);

    win = app_window(app, 640, 480, _("Mandelbrot"));
    if (!win)
        return 1;
    struct widget *bar = menubar_new(win);
    struct widget *file = menu_new(bar, _("File"));
    struct widget *m = menu_add(file, _("Save image..."), "save");
    widget_connect(m, "clicked", on_save, NULL);
    widget_set_accel(m, KEY_S, WMOD_CTRL);
    menu_add_separator(file);
    m = menu_add(file, _("Quit"), "quit");
    widget_connect(m, "clicked", on_quit, NULL);
    widget_set_accel(m, KEY_Q, WMOD_CTRL);
    struct widget *viewm = menu_new(bar, _("View"));
    widget_connect(menu_add(viewm, _("Zoom in"), "zoom-in"), "clicked", on_zoom_in, NULL);
    widget_connect(menu_add(viewm, _("Zoom out"), "zoom-out"), "clicked", on_zoom_out, NULL);
    widget_connect(menu_add(viewm, _("Reset view"), "fit"), "clicked", on_reset, NULL);
    menu_add_separator(viewm);
    mandel_item = menu_add(viewm, _("Mandelbrot set"), NULL);
    widget_connect(mandel_item, "clicked", on_mandel, NULL);
    widget_set_enabled(mandel_item, 0);
    julia_item = menu_add(viewm, _("Julia set of the centre"), NULL);
    widget_connect(julia_item, "clicked", on_julia, NULL);

    struct widget *tools = toolbar_new(win);
    widget_connect(toolbar_add(tools, "zoom-in", _("Zoom in")), "clicked", on_zoom_in, NULL);
    widget_connect(toolbar_add(tools, "zoom-out", _("Zoom out")), "clicked", on_zoom_out, NULL);
    widget_connect(toolbar_add(tools, "fit", _("Reset view")), "clicked", on_reset, NULL);
    widget_connect(toolbar_add(tools, "save", _("Save image")), "clicked", on_save, NULL);
    separator_new(tools);
    label_new(tools, _("Colours"));
    palette_box = combobox_new(tools);
    combobox_add(palette_box, _("Classic"));
    combobox_add(palette_box, _("Fire"));
    combobox_add(palette_box, _("Ocean"));
    combobox_add(palette_box, _("Grey"));
    combobox_select(palette_box, 0);
    widget_connect(palette_box, "changed", on_palette, NULL);
    label_new(tools, _("Iterations"));
    iter_box = combobox_new(tools);
    combobox_add(iter_box, _("Automatic"));
    combobox_add(iter_box, "256");
    combobox_add(iter_box, "1024");
    combobox_add(iter_box, "4096");
    combobox_select(iter_box, 0);
    widget_connect(iter_box, "changed", on_iterations, NULL);

    canvas = canvas_new(win);
    widget_set_stretch(canvas, 1, 1);
    widget_connect(canvas, "paint", on_paint, NULL);
    widget_connect(canvas, "press", on_press, NULL);
    widget_connect(canvas, "motion", on_motion, NULL);
    widget_connect(canvas, "release", on_release, NULL);
    widget_connect(canvas, "wheel", on_wheel, NULL);
    widget_connect(canvas, "key", on_key, NULL);

    struct widget *sb = statusbar_new(win);
    st_centre = statusbar_add(sb, 1);
    st_pointer = statusbar_add(sb, 0);
    widget_set_min(st_pointer, 160, 0);
    st_zoom = statusbar_add(sb, 0);
    widget_set_min(st_zoom, 70, 0);
    st_state = statusbar_add(sb, 0);
    widget_set_min(st_state, 150, 0);
    widget_focus(canvas);
    status_update();

    int code = app_run(app);
    render_stop();
    app_destroy(app);
    printf("mandel: exit %d\n", code);
    return code;
}
