/* view: an image viewer for every format a codec module decodes
 * (docs/design/codecs.md), such as PNG, BMP and SVG files. The window
 * shows one image scaled to fit or at a chosen zoom, steps through the
 * other images of its directory, and sets a raster image as the desktop
 * wallpaper.
 *
 *   view [FILE | DIRECTORY]
 *
 * Keys: Left, Page Up and Backspace show the previous image; Right, Page
 * Down and Space the next; Home and End the first and the last; + and -
 * zoom; 0 fits the image to the window and 1 shows it at its actual
 * size. The mouse wheel scrolls a large image, Shift with the wheel
 * scrolls it sideways, Ctrl with the wheel zooms, and dragging with the
 * left button moves it. */
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <codec/codec.h>
#include <gui/app.h>
#include <gui/mime.h>
#include <minios/input.h>

#define MAX_FILES 512
#define SVG_PX 1024             /* size at which vector formats are rendered */

static struct app *app;
static struct widget *win, *canvas, *st_name, *st_size, *st_zoom, *st_index, *wallpaper_item;

static char dir[PATH_MAX];
static char *files[MAX_FILES];
static int nfiles, current = -1;

static struct image *img;       /* the decoded file */
static struct image *reduced;   /* img resampled for a zoom below 100 % */
static int scalable;            /* img came from a vector format */
static int opaque;              /* every pixel of img has full alpha */
/* Zoom in device pixels per 1000 image pixels; 0 fits the image into
 * the window without enlarging it. */
static int zoom;
static int pan_x, pan_y;        /* logical pixels scrolled from the top left corner */
static int drag, drag_x, drag_y;

static const int levels[] = { 50, 100, 125, 167, 250, 333, 500, 667, 1000, 1500, 2000, 3000, 4000, 6000, 8000, 16000 };
#define NLEVELS ((int)(sizeof levels / sizeof levels[0]))

static int device_scale(void)
{
    struct window_state *ws = window_state_of(win);
    return ws && ws->win && ws->win->scale > 0 ? ws->win->scale : 1;
}

/* The zoom in effect: the fitted zoom when zoom is 0. */
static int effective_zoom(void)
{
    if (!img)
        return 1000;
    if (zoom)
        return zoom;
    long long s = device_scale();
    long long zx = (long long)canvas->w * s * 1000 / img->w, zy = (long long)canvas->h * s * 1000 / img->h;
    long long z = zx < zy ? zx : zy;
    return z > 1000 ? 1000 : z < 1 ? 1 : (int)z;
}

/* Size of the image on screen in logical pixels. */
static void shown_size(int *w, int *h)
{
    int z = effective_zoom(), s = device_scale();
    long long dw = (long long)img->w * z / 1000, dh = (long long)img->h * z / 1000;
    *w = (int)(dw / s > 0 ? dw / s : 1);
    *h = (int)(dh / s > 0 ? dh / s : 1);
}

static void clamp_pan(void)
{
    int w = 0, h = 0;
    if (img)
        shown_size(&w, &h);
    int max_x = w - canvas->w, max_y = h - canvas->h;
    pan_x = pan_x > max_x ? max_x : pan_x;
    pan_y = pan_y > max_y ? max_y : pan_y;
    pan_x = pan_x < 0 ? 0 : pan_x;
    pan_y = pan_y < 0 ? 0 : pan_y;
}

static int shown_zoom = -1;      /* the zoom in the status bar */

static void update_status(void)
{
    char text[64];
    shown_zoom = img ? effective_zoom() : -1;
    if (img) {
        snprintf(text, sizeof text, "%d x %d", img->w, img->h);
        widget_set_text(st_size, text);
        snprintf(text, sizeof text, "%d %%", (effective_zoom() + 5) / 10);
        widget_set_text(st_zoom, text);
    } else {
        widget_set_text(st_size, "");
        widget_set_text(st_zoom, "");
    }
    if (current >= 0)
        snprintf(text, sizeof text, "%d of %d", current + 1, nfiles);
    else
        text[0] = '\0';
    widget_set_text(st_index, text);
}

static void changed(void)
{
    clamp_pan();
    update_status();
    widget_invalidate(canvas);
}

static void set_zoom(int z)
{
    /* The point at the centre of the view remains at the centre. */
    int ow = 1, oh = 1, cx = 0, cy = 0;
    if (img) {
        shown_size(&ow, &oh);
        cx = pan_x + canvas->w / 2;
        cy = pan_y + canvas->h / 2;
    }
    zoom = z;
    image_free(reduced);
    reduced = NULL;
    if (img) {
        int nw, nh;
        shown_size(&nw, &nh);
        pan_x = (int)((long long)cx * nw / ow) - canvas->w / 2;
        pan_y = (int)((long long)cy * nh / oh) - canvas->h / 2;
    }
    changed();
}

static void zoom_step(int direction)
{
    int z = effective_zoom(), i;
    if (direction > 0) {
        for (i = 0; i < NLEVELS - 1 && levels[i] <= z; i++)
            ;
    } else {
        for (i = NLEVELS - 1; i > 0 && levels[i] >= z; i--)
            ;
    }
    set_zoom(levels[i]);
}

/* A name the viewer lists: one whose extension a codec decodes. */
static int has_image_extension(const char *name)
{
    return codec_for_path(CODEC_IMAGE, name, CODEC_DECODE) != NULL;
}

/* Decode a file with the codec selected by its content, or by its
 * extension when no probe matches. Vector formats render at SVG_PX. The
 * function sets scalable, and on failure it returns NULL and sets errno. */
static struct image *load_image(const char *path)
{
    uint8_t *data;
    size_t len;
    int err = codec_read_file(path, &data, &len);
    if (err < 0) {
        errno = -err;
        return NULL;
    }
    const struct codec *c = codec_identify(CODEC_IMAGE, data, len, path, CODEC_DECODE);
    struct codec_image_request req = { SVG_PX, SVG_PX, 0 };
    struct codec_picture pic;
    err = codec_image_decode(c, data, len, path, &req, &pic);
    free(data);
    struct image *im = err < 0 ? NULL : malloc(sizeof *im);
    if (!im) {
        if (err == 0)
            codec_picture_free(&pic);
        errno = err < 0 ? -err : ENOMEM;
        return NULL;
    }
    *im = (struct image){ pic.w, pic.h, pic.pixels, 1 };
    scalable = (c->caps & CODEC_SCALABLE) != 0;
    return im;
}

static int compare_names(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

/* The image files of a directory, sorted by name. */
static void scan_directory(const char *path)
{
    for (int i = 0; i < nfiles; i++)
        free(files[i]);
    nfiles = 0;
    strlcpy(dir, path, sizeof dir);
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL && nfiles < MAX_FILES)
        if (e->d_name[0] != '.' && has_image_extension(e->d_name))
            files[nfiles++] = strdup(e->d_name);
    closedir(d);
    qsort(files, (size_t)nfiles, sizeof files[0], compare_names);
}

static void file_path(int index, char *out, size_t size)
{
    snprintf(out, size, "%s/%s", strcmp(dir, "/") == 0 ? "" : dir, files[index]);
}

static void show(int index)
{
    image_free(img);
    image_free(reduced);
    img = reduced = NULL;
    current = index;
    pan_x = pan_y = 0;
    zoom = 0;
    char path[PATH_MAX], title[300];
    if (index < 0 || index >= nfiles) {
        current = -1;
        widget_set_text(st_name, "");
        gui_set_title(window_state_of(win)->win, "Viewer");
        widget_set_enabled(wallpaper_item, 0);
        changed();
        return;
    }
    file_path(index, path, sizeof path);
    errno = 0;
    img = load_image(path);
    opaque = 1;
    for (size_t i = 0, n = img ? (size_t)img->w * img->h : 0; i < n && opaque; i++)
        opaque = (img->pixels[i] >> 24) == 0xff;
    if (img) {
        widget_set_text(st_name, files[index]);
    } else {
        snprintf(title, sizeof title, "%s: %s", files[index], strerror(errno ? errno : EINVAL));
        widget_set_text(st_name, title);
    }
    widget_set_enabled(wallpaper_item, img && !scalable);
    gui_set_title(window_state_of(win)->win, files[index]);
    changed();
}

/* Show a file, or the first image of a directory. Returns 0 or -errno. */
static int open_path(const char *name)
{
    char full[PATH_MAX];
    if (!realpath(name, full))
        return -errno;
    DIR *d = opendir(full);
    if (d) {
        closedir(d);
        scan_directory(full);
        show(nfiles ? 0 : -1);
        return 0;
    }
    if (!has_image_extension(full))
        return -EINVAL;
    char *slash = strrchr(full, '/');
    *slash = '\0';
    scan_directory(slash == full ? "/" : full);
    for (int i = 0; i < nfiles; i++)
        if (strcmp(files[i], slash + 1) == 0) {
            show(i);
            return 0;
        }
    return -ENOENT;
}

/* ---- painting ---- */

static void checkerboard(struct painter *p, int x, int y, int w, int h)
{
    struct rect clip = painter_clip_local(p);
    struct rect r = rect_intersect((struct rect){ x, y, w, h }, clip);
    for (int j = (r.y - y) / 8 * 8; j < r.y + r.h - y; j += 8)
        for (int i = (r.x - x) / 8 * 8; i < r.x + r.w - x; i += 8) {
            int cw = i + 8 > w ? w - i : 8, ch = j + 8 > h ? h - j : 8;
            painter_fill(p, x + i, y + j, cw, ch, ((i ^ j) & 8) ? 0x00c8c8c8 : 0x00f0f0f0);
        }
}

static int on_paint(struct widget *w, void *args, void *arg)
{
    struct painter *p = ((struct sig_paint *)args)->p;
    painter_fill(p, 0, 0, w->w, w->h, 0x00303030);
    if (!img)
        return 1;
    /* The fitted zoom depends on the canvas size, which is known only
     * after the layout. */
    if (effective_zoom() != shown_zoom)
        update_status();
    int dw, dh;
    shown_size(&dw, &dh);
    int x = dw < w->w ? (w->w - dw) / 2 : -pan_x, y = dh < w->h ? (w->h - dh) / 2 : -pan_y;
    if (!opaque)
        checkerboard(p, x, y, dw, dh);
    if (effective_zoom() < 1000) {
        /* Reductions are resampled once per zoom, at device resolution. */
        int s = p->scale;
        if (!reduced || reduced->w != dw * s || reduced->h != dh * s) {
            image_free(reduced);
            reduced = image_scale(img, dw * s, dh * s);
        }
        if (reduced) {
            painter_image_scaled(p, x, y, dw, dh, reduced);
            return 1;
        }
    }
    painter_image_scaled(p, x, y, dw, dh, img);
    return 1;
}

/* ---- input ---- */

static int on_press(struct widget *w, void *args, void *arg)
{
    struct sig_click *c = args;
    widget_focus(canvas);
    if (c->button & 1) {
        drag = 1;
        drag_x = c->x;
        drag_y = c->y;
    }
    return 1;
}

static int on_release(struct widget *w, void *args, void *arg)
{
    drag = 0;
    return 1;
}

static int on_motion(struct widget *w, void *args, void *arg)
{
    struct sig_click *c = args;
    if (!drag || !(c->button & 1))
        return 0;
    pan_x -= c->x - drag_x;
    pan_y -= c->y - drag_y;
    drag_x = c->x;
    drag_y = c->y;
    changed();
    return 1;
}

static int on_wheel(struct widget *w, void *args, void *arg)
{
    int delta = ((struct sig_click *)args)->button, mods = gui_modifiers();
    if (mods & WMOD_CTRL)
        zoom_step(delta < 0 ? 1 : -1);
    else if (mods & WMOD_SHIFT)
        pan_x += delta * 40;
    else
        pan_y += delta * 40;
    changed();
    return 1;
}

static void step(int delta)
{
    if (nfiles == 0)
        return;
    int i = current + delta;
    show(i < 0 ? 0 : i >= nfiles ? nfiles - 1 : i);
}

static int on_key(struct widget *w, void *args, void *arg)
{
    struct sig_key *k = args;
    switch (k->code) {
    case KEY_LEFT: case KEY_PAGEUP: case KEY_BACKSPACE: step(-1); return 1;
    case KEY_RIGHT: case KEY_PAGEDOWN: case KEY_SPACE: step(1); return 1;
    case KEY_HOME: show(nfiles ? 0 : -1); return 1;
    case KEY_END: show(nfiles - 1); return 1;
    case KEY_UP: pan_y -= 40; changed(); return 1;
    case KEY_DOWN: pan_y += 40; changed(); return 1;
    }
    if (k->ch == '+' || k->ch == '=')
        zoom_step(1);
    else if (k->ch == '-')
        zoom_step(-1);
    else if (k->ch == '0')
        set_zoom(0);
    else if (k->ch == '1')
        set_zoom(1000);
    else
        return 0;
    return 1;
}

static int on_resize(struct widget *w, void *args, void *arg)
{
    changed();
    return 0;
}

/* ---- commands ---- */

static void error_dialog(const char *text)
{
    const char *const buttons[] = { "OK" };
    app_dialog(app, "Viewer", text, buttons, 1);
}

static int on_open(struct widget *w, void *args, void *arg)
{
    char name[PATH_MAX];
    if (current >= 0)
        file_path(current, name, sizeof name);
    else
        snprintf(name, sizeof name, "%s/", getenv("HOME") ? getenv("HOME") : "/home");
    if (app_prompt(app, "Open", "File:", name, sizeof name) && open_path(name) < 0)
        error_dialog("The file cannot be opened.");
    return 1;
}

static int on_wallpaper(struct widget *w, void *args, void *arg)
{
    if (current < 0 || !img || scalable)
        return 1;
    char path[PATH_MAX];
    file_path(current, path, sizeof path);
    char *const argv[] = { "/bin/settings", "set", "wallpaper", path, NULL };
    if (mime_spawn(argv) < 0)
        error_dialog("The settings program cannot be started.");
    return 1;
}

static int on_quit(struct widget *w, void *args, void *arg) { app_quit(app, 0); return 1; }
static int on_prev(struct widget *w, void *args, void *arg) { step(-1); return 1; }
static int on_next(struct widget *w, void *args, void *arg) { step(1); return 1; }
static int on_first(struct widget *w, void *args, void *arg) { show(nfiles ? 0 : -1); return 1; }
static int on_last(struct widget *w, void *args, void *arg) { show(nfiles - 1); return 1; }
static int on_zoom_in(struct widget *w, void *args, void *arg) { zoom_step(1); return 1; }
static int on_zoom_out(struct widget *w, void *args, void *arg) { zoom_step(-1); return 1; }
static int on_actual(struct widget *w, void *args, void *arg) { set_zoom(1000); return 1; }
static int on_fit(struct widget *w, void *args, void *arg) { set_zoom(0); return 1; }

int main(int argc, char **argv)
{
    app = app_create();
    if (!app)
        return 1;
    win = app_window(app, 640, 480, "Viewer");
    if (!win)
        return 1;
    widget_connect(win, "resize", on_resize, NULL);
    struct widget *bar = menubar_new(win);
    struct widget *file = menu_new(bar, "File");
    widget_connect(menu_add(file, "Open...", "open"), "clicked", on_open, NULL);
    wallpaper_item = menu_add(file, "Set as wallpaper", "wallpaper");
    widget_connect(wallpaper_item, "clicked", on_wallpaper, NULL);
    menu_add_separator(file);
    widget_connect(menu_add(file, "Quit", "quit"), "clicked", on_quit, NULL);
    struct widget *vw = menu_new(bar, "View");
    widget_connect(menu_add(vw, "Zoom in", "zoom-in"), "clicked", on_zoom_in, NULL);
    widget_connect(menu_add(vw, "Zoom out", "zoom-out"), "clicked", on_zoom_out, NULL);
    widget_connect(menu_add(vw, "Actual size", NULL), "clicked", on_actual, NULL);
    widget_connect(menu_add(vw, "Best fit", "fit"), "clicked", on_fit, NULL);
    struct widget *go = menu_new(bar, "Go");
    widget_connect(menu_add(go, "Previous", "back"), "clicked", on_prev, NULL);
    widget_connect(menu_add(go, "Next", "forward"), "clicked", on_next, NULL);
    widget_connect(menu_add(go, "First", NULL), "clicked", on_first, NULL);
    widget_connect(menu_add(go, "Last", NULL), "clicked", on_last, NULL);

    struct widget *tools = toolbar_new(win);
    struct widget *b = toolbar_add(tools, "open", "Open");
    widget_connect(b, "clicked", on_open, NULL);
    widget_set_accel(b, KEY_O, WMOD_CTRL);
    widget_connect(toolbar_add(tools, "back", "Previous"), "clicked", on_prev, NULL);
    widget_connect(toolbar_add(tools, "forward", "Next"), "clicked", on_next, NULL);
    widget_connect(toolbar_add(tools, "zoom-out", "Zoom out"), "clicked", on_zoom_out, NULL);
    widget_connect(toolbar_add(tools, "zoom-in", "Zoom in"), "clicked", on_zoom_in, NULL);
    widget_connect(toolbar_add(tools, "fit", "Best fit"), "clicked", on_fit, NULL);

    canvas = canvas_new(win);
    widget_set_stretch(canvas, 1, 1);
    canvas->focusable = 1;
    widget_connect(canvas, "paint", on_paint, NULL);
    widget_connect(canvas, "press", on_press, NULL);
    widget_connect(canvas, "release", on_release, NULL);
    widget_connect(canvas, "motion", on_motion, NULL);
    widget_connect(canvas, "wheel", on_wheel, NULL);
    widget_connect(canvas, "key", on_key, NULL);
    struct widget *sb = statusbar_new(win);
    st_name = statusbar_add(sb, 1);
    st_size = statusbar_add(sb, 0);
    widget_set_min(st_size, 100, 0);
    st_zoom = statusbar_add(sb, 0);
    widget_set_min(st_zoom, 60, 0);
    st_index = statusbar_add(sb, 0);
    widget_set_min(st_index, 70, 0);
    widget_set_enabled(wallpaper_item, 0);
    if (argc > 1) {
        int err = open_path(argv[1]);
        if (err < 0) {
            fprintf(stderr, "view: cannot open %s: %s\n", argv[1], strerror(-err));
            return 1;
        }
    }
    widget_focus(canvas);
    app_run(app);
    app_destroy(app);
    return 0;
}
