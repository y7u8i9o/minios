/* paint: a bitmap editor for PNG files with a brush, an eraser, lines,
 * rectangles, ellipses, flood fill, a palette, undo and redo.
 *
 *   paint [FILE]
 *
 * A FILE that does not exist is created by the first save. Keys: B brush,
 * E eraser, L line, R rectangle, O ellipse, F fill; + and - change the
 * brush size; Ctrl+N new, Ctrl+O open, Ctrl+S save, Ctrl+Z undo, Ctrl+Y
 * redo. The image has one pixel per logical pixel of the window. */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gui/app.h>
#include <minios/input.h>

#define NEW_W 640
#define NEW_H 480
#define MAX_SIZE 4096
#define UNDO_MAX 8
#define WHITE 0x00ffffff

enum tool { TOOL_BRUSH, TOOL_ERASER, TOOL_LINE, TOOL_RECT, TOOL_ELLIPSE, TOOL_FILL };
static const char *const tool_names[] = { "Brush", "Eraser", "Line", "Rectangle", "Ellipse", "Fill" };

static const uint32_t palette[] = {
    0x00000000, 0x00808080, 0x00c0c0c0, 0x00ffffff, 0x00800000, 0x00e03030, 0x00f08030, 0x00e0c020,
    0x00308030, 0x0030c050, 0x00208080, 0x0030b0e0, 0x00203080, 0x003060e0, 0x00602080, 0x00c040c0,
};
#define NCOLORS ((int)(sizeof palette / sizeof palette[0]))
#define SWATCH 20

static struct app *app;
static struct widget *win, *area, *canvas, *swatches, *size_spin, *fill_box;
static struct widget *st_tool, *st_pos, *st_size, *undo_item, *redo_item;

static struct surface img;      /* the drawing, 0x00RRGGBB */
static char path[PATH_MAX];     /* empty until the first save */
static int modified;
static enum tool tool;
static uint32_t color;
static int size = 4, filled;

/* The undo and redo stacks contain whole copies of the drawing, newest last. */
static struct surface undo_stack[UNDO_MAX], redo_stack[UNDO_MAX];
static int nundo, nredo;
static struct surface before;   /* the drawing when the current stroke started */
static int stroke, sx, sy, lx, ly;

/* ---- surfaces ---- */

static struct surface surface_copy(const struct surface *s)
{
    struct surface c = { malloc((size_t)s->width * s->height * 4), s->width, s->height, s->width };
    if (c.pixels)
        memcpy(c.pixels, s->pixels, (size_t)s->width * s->height * 4);
    else
        c.width = c.height = 0;
    return c;
}

static void surface_release(struct surface *s)
{
    free(s->pixels);
    *s = (struct surface){ 0 };
}

static void push(struct surface *stack, int *n, struct surface s)
{
    if (*n == UNDO_MAX) {
        surface_release(&stack[0]);
        memmove(stack, stack + 1, sizeof stack[0] * (UNDO_MAX - 1));
        (*n)--;
    }
    stack[(*n)++] = s;
}

static void clear_stack(struct surface *stack, int *n)
{
    while (*n > 0)
        surface_release(&stack[--*n]);
}

/* ---- window state ---- */

static void update_title(void)
{
    char title[PATH_MAX + 16];
    const char *base = strrchr(path, '/');
    snprintf(title, sizeof title, "%s%s", modified ? "*" : "", path[0] ? (base ? base + 1 : path) : "Untitled");
    gui_set_title(window_state_of(win)->win, title);
}

static void update_status(void)
{
    char text[64];
    snprintf(text, sizeof text, "%s, %d px", tool_names[tool], size);
    widget_set_text(st_tool, text);
    snprintf(text, sizeof text, "%d x %d", img.width, img.height);
    widget_set_text(st_size, text);
    widget_set_enabled(undo_item, nundo > 0);
    widget_set_enabled(redo_item, nredo > 0);
}

static void set_modified(int on)
{
    if (modified != on) {
        modified = on;
        update_title();
    }
}

/* A new drawing size: the canvas takes the size of the image and the
 * scroll area shows bars when it does not fit. */
static void resize_canvas(void)
{
    widget_set_hint(canvas, img.width, img.height);
    widget_set_min(canvas, img.width, img.height);
    widget_set_max(canvas, img.width, img.height);
    widget_relayout(area);
    widget_invalidate(canvas);
    update_status();
}

static void replace_image(struct surface s)
{
    surface_release(&img);
    img = s;
    clear_stack(undo_stack, &nundo);
    clear_stack(redo_stack, &nredo);
    resize_canvas();
}

/* ---- drawing ---- */

static void disc(int cx, int cy, int d, uint32_t c)
{
    if (d <= 2) {
        gfx_fill_rect(&img, cx - d / 2, cy - d / 2, d, d, c);
        return;
    }
    int r2 = d * d;             /* (2r)^2, so that even diameters are centred */
    for (int j = 0; j < d; j++) {
        int y2 = 2 * j - d + 1;
        for (int i = 0; i < d; i++) {
            int x2 = 2 * i - d + 1;
            if (x2 * x2 + y2 * y2 <= r2)
                gfx_fill_rect(&img, cx - d / 2 + i, cy - d / 2 + j, 1, 1, c);
        }
    }
}

static void stroke_line(int x0, int y0, int x1, int y1, int d, uint32_t c)
{
    int dx = x1 - x0, dy = y1 - y0;
    int steps = abs(dx) > abs(dy) ? abs(dx) : abs(dy);
    if (steps == 0)
        steps = 1;
    for (int i = 0; i <= steps; i++)
        disc(x0 + dx * i / steps, y0 + dy * i / steps, d, c);
}

static void draw_rect(int x0, int y0, int x1, int y1)
{
    int l = x0 < x1 ? x0 : x1, r = x0 < x1 ? x1 : x0, t = y0 < y1 ? y0 : y1, b = y0 < y1 ? y1 : y0;
    if (filled) {
        gfx_fill_rect(&img, l, t, r - l + 1, b - t + 1, color);
        return;
    }
    stroke_line(l, t, r, t, size, color);
    stroke_line(r, t, r, b, size, color);
    stroke_line(r, b, l, b, size, color);
    stroke_line(l, b, l, t, size, color);
}

/* The ellipse inside the rectangle of the two corners, by its rows: each
 * row spans the x range where (x/a)^2 + (y/b)^2 <= 1, in integers scaled
 * by 4 a^2 b^2. The outline joins the ends of consecutive rows. */
static void draw_ellipse(int x0, int y0, int x1, int y1)
{
    int l = x0 < x1 ? x0 : x1, r = x0 < x1 ? x1 : x0, t = y0 < y1 ? y0 : y1, b = y0 < y1 ? y1 : y0;
    long long w = r - l, h = b - t;
    if (w == 0 || h == 0) {
        stroke_line(l, t, r, b, size, color);
        return;
    }
    int px0 = 0, px1 = 0, have = 0;
    for (int y = t; y <= b; y++) {
        long long ry = 2 * (y - t) - h;     /* 2 (y - centre) */
        long long rem = h * h - ry * ry;    /* scaled 1 - (y/b)^2 */
        if (rem < 0)
            rem = 0;
        /* half width: sqrt(w^2 rem / h^2) / 2 in pixels */
        long long target = w * w * rem / (h * h), half = 0;
        while ((half + 1) * (half + 1) <= target)
            half++;
        int cx2 = l + r, a = (int)((cx2 - half) / 2), z = (int)((cx2 + half + 1) / 2);
        if (filled) {
            gfx_fill_rect(&img, a, y, z - a + 1, 1, color);
        } else {
            if (have) {
                stroke_line(px0, y - 1, a, y, size, color);
                stroke_line(px1, y - 1, z, y, size, color);
            } else {
                stroke_line(a, y, z, y, size, color);
            }
            if (y == b)
                stroke_line(a, y, z, y, size, color);
            px0 = a;
            px1 = z;
            have = 1;
        }
    }
}

/* Scanline flood fill of the region of the colour at (x, y). */
static void flood_fill(int x, int y, uint32_t c)
{
    if (x < 0 || y < 0 || x >= img.width || y >= img.height)
        return;
    uint32_t *px = img.pixels;
    int w = img.width, h = img.height;
    uint32_t target = px[(size_t)y * w + x];
    if (target == c)
        return;
    size_t cap = 1024, n = 0;
    int *stack = malloc(cap * 2 * sizeof(int));
    if (!stack)
        return;
    stack[n * 2] = x;
    stack[n * 2 + 1] = y;
    n++;
    while (n > 0) {
        n--;
        int cx = stack[n * 2], cy = stack[n * 2 + 1];
        uint32_t *row = px + (size_t)cy * w;
        if (row[cx] != target)
            continue;
        int a = cx, z = cx;
        while (a > 0 && row[a - 1] == target)
            a--;
        while (z < w - 1 && row[z + 1] == target)
            z++;
        for (int i = a; i <= z; i++)
            row[i] = c;
        for (int dir = -1; dir <= 1; dir += 2) {
            int ny = cy + dir;
            if (ny < 0 || ny >= h)
                continue;
            uint32_t *nrow = px + (size_t)ny * w;
            for (int i = a; i <= z; i++) {
                if (nrow[i] != target || (i > a && nrow[i - 1] == target))
                    continue;
                if (n == cap) {
                    int *grown = realloc(stack, cap * 4 * sizeof(int));
                    if (!grown) {
                        free(stack);
                        return;
                    }
                    stack = grown;
                    cap *= 2;
                }
                stack[n * 2] = i;
                stack[n * 2 + 1] = ny;
                n++;
            }
        }
    }
    free(stack);
}

/* Shapes are drawn again from the copy taken at the press on every
 * motion, so that the drawing shows the shape that a release would
 * leave. */
static void draw_shape(int x, int y)
{
    memcpy(img.pixels, before.pixels, (size_t)img.width * img.height * 4);
    if (tool == TOOL_LINE)
        stroke_line(sx, sy, x, y, size, color);
    else if (tool == TOOL_RECT)
        draw_rect(sx, sy, x, y);
    else
        draw_ellipse(sx, sy, x, y);
}

/* ---- canvas ---- */

static int on_paint(struct widget *w, void *args, void *arg)
{
    struct painter *p = ((struct sig_paint *)args)->p;
    painter_blit(p, 0, 0, &img);
    if (w->w > img.width)
        painter_fill(p, img.width, 0, w->w - img.width, w->h, p->theme->color[TC_FIELD]);
    if (w->h > img.height)
        painter_fill(p, 0, img.height, img.width, w->h - img.height, p->theme->color[TC_FIELD]);
    return 1;
}

static void show_position(int x, int y)
{
    char text[32];
    if (x >= 0 && y >= 0 && x < img.width && y < img.height)
        snprintf(text, sizeof text, "%d, %d", x, y);
    else
        text[0] = '\0';
    widget_set_text(st_pos, text);
}

static int on_press(struct widget *w, void *args, void *arg)
{
    struct sig_click *c = args;
    widget_focus(canvas);
    if (!(c->button & 1) || stroke)
        return 1;
    surface_release(&before);
    before = surface_copy(&img);
    if (!before.pixels)
        return 1;
    stroke = 1;
    sx = lx = c->x;
    sy = ly = c->y;
    if (tool == TOOL_BRUSH || tool == TOOL_ERASER)
        disc(c->x, c->y, size, tool == TOOL_ERASER ? WHITE : color);
    else if (tool == TOOL_FILL)
        flood_fill(c->x, c->y, color);
    else
        draw_shape(c->x, c->y);
    widget_invalidate(canvas);
    return 1;
}

static int on_motion(struct widget *w, void *args, void *arg)
{
    struct sig_click *c = args;
    show_position(c->x, c->y);
    if (!stroke)
        return 1;
    if (tool == TOOL_BRUSH || tool == TOOL_ERASER)
        stroke_line(lx, ly, c->x, c->y, size, tool == TOOL_ERASER ? WHITE : color);
    else if (tool != TOOL_FILL)
        draw_shape(c->x, c->y);
    lx = c->x;
    ly = c->y;
    widget_invalidate(canvas);
    return 1;
}

/* The release ends the stroke: the copy taken at the press becomes the
 * undo step. */
static int on_release(struct widget *w, void *args, void *arg)
{
    if (!stroke)
        return 1;
    stroke = 0;
    push(undo_stack, &nundo, before);
    before = (struct surface){ 0 };
    clear_stack(redo_stack, &nredo);
    set_modified(1);
    update_status();
    return 1;
}

/* ---- palette ---- */

static int on_swatch_paint(struct widget *w, void *args, void *arg)
{
    struct painter *p = ((struct sig_paint *)args)->p;
    uint32_t border = p->theme->color[TC_BORDER];
    painter_fill(p, 0, 0, w->w, w->h, p->theme->color[TC_WINDOW]);
    /* The current colour, then the palette. */
    painter_fill(p, 2, 2, SWATCH * 2, SWATCH, color);
    painter_frame(p, 2, 2, SWATCH * 2, SWATCH, border);
    for (int i = 0; i < NCOLORS; i++) {
        int x = 2 + SWATCH * 2 + 8 + i * (SWATCH + 2);
        painter_fill(p, x, 2, SWATCH, SWATCH, palette[i]);
        painter_frame(p, x, 2, SWATCH, SWATCH, palette[i] == color ? p->theme->color[TC_ACCENT] : border);
    }
    return 1;
}

static int on_swatch_press(struct widget *w, void *args, void *arg)
{
    struct sig_click *c = args;
    int i = (c->x - (2 + SWATCH * 2 + 8)) / (SWATCH + 2);
    if (c->x >= 2 + SWATCH * 2 + 8 && i >= 0 && i < NCOLORS && c->y >= 2 && c->y < 2 + SWATCH) {
        color = palette[i];
        widget_invalidate(swatches);
    }
    return 1;
}

/* ---- commands ---- */

static void error_dialog(const char *fmt, const char *name, int err)
{
    char text[PATH_MAX + 128];
    snprintf(text, sizeof text, fmt, name, strerror(err));
    const char *const buttons[] = { "OK" };
    app_dialog(app, "Paint", text, buttons, 1);
}

static int save_to(const char *name)
{
    struct image out = { img.width, img.height, malloc((size_t)img.width * img.height * 4), 1 };
    if (!out.pixels) {
        error_dialog("%s cannot be saved: %s", name, ENOMEM);
        return -ENOMEM;
    }
    for (size_t i = 0, n = (size_t)img.width * img.height; i < n; i++)
        out.pixels[i] = img.pixels[i] | 0xff000000u;
    int err = image_save_png(&out, name);
    free(out.pixels);
    if (err < 0) {
        error_dialog("%s cannot be saved: %s", name, -err);
        return err;
    }
    if (name != path)
        strlcpy(path, name, sizeof path);
    modified = 0;
    update_title();
    printf("paint: saved %s\n", path);
    fflush(stdout);
    return 0;
}

/* save_as and save return 0 when the file was written, 1 when the name
 * prompt was cancelled, or a negative errno. */
static int save_as(void)
{
    char name[PATH_MAX];
    if (path[0])
        strlcpy(name, path, sizeof name);
    else
        snprintf(name, sizeof name, "%s/Pictures/untitled.png", getenv("HOME") ? getenv("HOME") : "/home");
    if (!app_prompt(app, "Save as", "File:", name, sizeof name))
        return 1;
    return save_to(name);
}

static int save(void)
{
    return path[0] ? save_to(path) : save_as();
}

/* Before the drawing is replaced or the window closes: 1 to go on. */
static int confirm_discard(void)
{
    if (!modified)
        return 1;
    const char *const buttons[] = { "Save", "Discard", "Cancel" };
    int choice = app_dialog(app, "Paint", "The drawing has unsaved changes.", buttons, 3);
    if (choice == 0)
        return save() == 0;
    return choice == 1;
}

/* A PNG file, composed over white because the drawing has no alpha. */
static int load(const char *name)
{
    struct image *im = image_load(name);
    if (!im)
        return -errno;
    if (im->w > MAX_SIZE || im->h > MAX_SIZE) {
        image_free(im);
        return -EFBIG;
    }
    struct surface s = { malloc((size_t)im->w * im->h * 4), im->w, im->h, im->w };
    if (!s.pixels) {
        image_free(im);
        return -ENOMEM;
    }
    for (size_t i = 0, n = (size_t)im->w * im->h; i < n; i++) {
        uint32_t c = im->pixels[i], a = c >> 24, out = 0;
        for (int sh = 0; sh < 24; sh += 8) {
            uint32_t v = (c >> sh) & 0xff;
            out |= ((v * a + 255 * (255 - a) + 127) / 255) << sh;
        }
        s.pixels[i] = out;
    }
    image_free(im);
    replace_image(s);
    strlcpy(path, name, sizeof path);
    modified = 0;
    update_title();
    return 0;
}

static void new_image(int w, int h)
{
    struct surface s = { malloc((size_t)w * h * 4), w, h, w };
    if (!s.pixels)
        return;
    gfx_fill(&s, WHITE);
    replace_image(s);
    path[0] = '\0';
    modified = 0;
    update_title();
}

static void undo_redo(struct surface *from, int *nfrom, struct surface *to, int *nto)
{
    if (*nfrom == 0)
        return;
    struct surface cur = img;
    img = from[--*nfrom];
    push(to, nto, cur);
    if (img.width != cur.width || img.height != cur.height)
        resize_canvas();
    widget_invalidate(canvas);
    set_modified(1);
    update_status();
}

static int on_new(struct widget *w, void *args, void *arg)
{
    if (!confirm_discard())
        return 1;
    char text[32];
    snprintf(text, sizeof text, "%dx%d", img.width, img.height);
    if (!app_prompt(app, "New", "Size:", text, sizeof text))
        return 1;
    int nw = 0, nh = 0;
    if (sscanf(text, "%dx%d", &nw, &nh) != 2 || nw < 1 || nh < 1 || nw > MAX_SIZE || nh > MAX_SIZE) {
        const char *const buttons[] = { "OK" };
        app_dialog(app, "Paint", "The size must be WIDTHxHEIGHT with both values from 1 to 4096.", buttons, 1);
        return 1;
    }
    new_image(nw, nh);
    return 1;
}

static int on_open(struct widget *w, void *args, void *arg)
{
    if (!confirm_discard())
        return 1;
    char name[PATH_MAX];
    if (path[0])
        strlcpy(name, path, sizeof name);
    else
        snprintf(name, sizeof name, "%s/", getenv("HOME") ? getenv("HOME") : "/home");
    if (!app_prompt(app, "Open", "File:", name, sizeof name))
        return 1;
    int err = load(name);
    if (err < 0)
        error_dialog("%s cannot be opened: %s", name, -err);
    return 1;
}

static int on_save(struct widget *w, void *args, void *arg) { save(); return 1; }
static int on_save_as(struct widget *w, void *args, void *arg) { save_as(); return 1; }
static int on_undo(struct widget *w, void *args, void *arg) { undo_redo(undo_stack, &nundo, redo_stack, &nredo); return 1; }
static int on_redo(struct widget *w, void *args, void *arg) { undo_redo(redo_stack, &nredo, undo_stack, &nundo); return 1; }

static int on_quit(struct widget *w, void *args, void *arg)
{
    if (confirm_discard())
        app_quit(app, 0);
    return 1;
}

/* The close button: a return value of 1 prevents the window from closing. */
static int on_close(struct widget *w, void *args, void *arg)
{
    return !confirm_discard();
}

static int on_tool(struct widget *w, void *args, void *arg)
{
    tool = (enum tool)(long)arg;
    update_status();
    return 1;
}

static int on_size(struct widget *w, void *args, void *arg)
{
    size = ((struct sig_change *)args)->value;
    update_status();
    return 1;
}

static int on_filled(struct widget *w, void *args, void *arg)
{
    filled = w->value;
    return 1;
}

static int on_key(struct widget *w, void *args, void *arg)
{
    struct sig_key *k = args;
    if (k->mods & (WMOD_CTRL | WMOD_ALT))
        return 0;
    switch (k->ch) {
    case 'b': tool = TOOL_BRUSH; break;
    case 'e': tool = TOOL_ERASER; break;
    case 'l': tool = TOOL_LINE; break;
    case 'r': tool = TOOL_RECT; break;
    case 'o': tool = TOOL_ELLIPSE; break;
    case 'f': tool = TOOL_FILL; break;
    case '+': case '=': widget_set_value(size_spin, size < 64 ? size + 1 : size); size = size_spin->value; break;
    case '-': widget_set_value(size_spin, size > 1 ? size - 1 : size); size = size_spin->value; break;
    default: return 0;
    }
    update_status();
    return 1;
}

static struct widget *tool_button(struct widget *bar, const char *icon, const char *tip, enum tool t)
{
    struct widget *b = toolbar_add(bar, icon, tip);
    widget_connect(b, "clicked", on_tool, (void *)(long)t);
    return b;
}

int main(int argc, char **argv)
{
    app = app_create();
    if (!app)
        return 1;
    win = app_window(app, 720, 540, "Untitled");
    if (!win)
        return 1;
    widget_connect(win, "close", on_close, NULL);
    struct widget *bar = menubar_new(win);
    struct widget *file = menu_new(bar, "File");
    widget_connect(menu_add(file, "New...", "new"), "clicked", on_new, NULL);
    widget_connect(menu_add(file, "Open...", "open"), "clicked", on_open, NULL);
    widget_connect(menu_add(file, "Save", "save"), "clicked", on_save, NULL);
    widget_connect(menu_add(file, "Save as...", NULL), "clicked", on_save_as, NULL);
    menu_add_separator(file);
    widget_connect(menu_add(file, "Quit", "quit"), "clicked", on_quit, NULL);
    struct widget *edit = menu_new(bar, "Edit");
    undo_item = menu_add(edit, "Undo", "undo");
    widget_connect(undo_item, "clicked", on_undo, NULL);
    redo_item = menu_add(edit, "Redo", "redo");
    widget_connect(redo_item, "clicked", on_redo, NULL);
    struct widget *tools_menu = menu_new(bar, "Tools");
    static const char *const icons[] = { "paint", "eraser", "line", "rectangle", "ellipse", "fill" };
    for (int i = 0; i <= TOOL_FILL; i++)
        widget_connect(menu_add(tools_menu, tool_names[i], icons[i]), "clicked", on_tool, (void *)(long)i);

    struct widget *tb = toolbar_new(win);
    struct widget *b = toolbar_add(tb, "new", "New");
    widget_connect(b, "clicked", on_new, NULL);
    widget_set_accel(b, KEY_N, WMOD_CTRL);
    b = toolbar_add(tb, "open", "Open");
    widget_connect(b, "clicked", on_open, NULL);
    widget_set_accel(b, KEY_O, WMOD_CTRL);
    b = toolbar_add(tb, "save", "Save");
    widget_connect(b, "clicked", on_save, NULL);
    widget_set_accel(b, KEY_S, WMOD_CTRL);
    b = toolbar_add(tb, "undo", "Undo");
    widget_connect(b, "clicked", on_undo, NULL);
    widget_set_accel(b, KEY_Z, WMOD_CTRL);
    b = toolbar_add(tb, "redo", "Redo");
    widget_connect(b, "clicked", on_redo, NULL);
    widget_set_accel(b, KEY_Y, WMOD_CTRL);
    for (int i = 0; i <= TOOL_FILL; i++)
        tool_button(tb, icons[i], tool_names[i], (enum tool)i);
    size_spin = spinner_new(tb, 1, 64, size);
    widget_set_tip(size_spin, "Brush size");
    widget_connect(size_spin, "changed", on_size, NULL);
    fill_box = checkbox_new(tb, "Filled");
    widget_connect(fill_box, "toggled", on_filled, NULL);

    area = scrollarea_new(win);
    canvas = canvas_new((struct widget *)area->user);
    widget_set_align(canvas, ALIGN_START, ALIGN_START);
    canvas->focusable = 1;
    widget_connect(canvas, "paint", on_paint, NULL);
    widget_connect(canvas, "press", on_press, NULL);
    widget_connect(canvas, "motion", on_motion, NULL);
    widget_connect(canvas, "release", on_release, NULL);
    widget_connect(canvas, "key", on_key, NULL);

    swatches = canvas_new(win);
    widget_set_hint(swatches, 2 + SWATCH * 2 + 8 + NCOLORS * (SWATCH + 2), SWATCH + 4);
    widget_set_stretch(swatches, 1, 0);
    widget_set_max(swatches, 0, SWATCH + 4);
    widget_connect(swatches, "paint", on_swatch_paint, NULL);
    widget_connect(swatches, "press", on_swatch_press, NULL);

    struct widget *sb = statusbar_new(win);
    st_tool = statusbar_add(sb, 1);
    st_pos = statusbar_add(sb, 0);
    widget_set_min(st_pos, 80, 0);
    st_size = statusbar_add(sb, 0);
    widget_set_min(st_size, 80, 0);

    new_image(NEW_W, NEW_H);
    if (argc > 1) {
        int err = load(argv[1]);
        if (err == -ENOENT) {
            strlcpy(path, argv[1], sizeof path);
            update_title();
        } else if (err < 0) {
            fprintf(stderr, "paint: cannot open %s: %s\n", argv[1], strerror(-err));
            return 1;
        }
    }
    widget_focus(canvas);
    app_run(app);
    app_destroy(app);
    return 0;
}
