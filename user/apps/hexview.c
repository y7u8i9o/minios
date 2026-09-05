/* hexview [path]: a hex dump of a file or device, 16 bytes per line,
 * navigated by an offset field, page keys and a scroll bar. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <gui/app.h>

#define LINES 24
#define ROW 16

static struct app *app;
static struct widget *canvas, *offset_field, *status, *bar;
static int fd = -1;
static long size, offset;
static char path[256];

static long file_size(void)
{
    long end = lseek(fd, 0, SEEK_END);
    return end > 0 ? end : 0;
}

static void go_to(long off)
{
    if (off < 0) off = 0;
    if (size && off > size - ROW) off = (size - ROW) / ROW * ROW;
    if (off < 0) off = 0;
    offset = off / ROW * ROW;
    char s[32];
    snprintf(s, sizeof s, "%lx", offset);
    widget_set_text(offset_field, s);
    scrollbar_set(bar, (int)(offset / ROW), (int)((size + ROW - 1) / ROW), LINES);
    widget_invalidate(canvas);
}

static int on_paint(struct widget *w, void *args, void *arg)
{
    struct painter *p = ((struct sig_paint *)args)->p;
    painter_fill(p, 0, 0, w->w, w->h, 0x00ffffff);
    const struct font *f = gfx_font_builtin();
    unsigned char buf[LINES * ROW];
    long n = 0;
    if (fd >= 0 && lseek(fd, offset, SEEK_SET) >= 0)
        n = read(fd, buf, sizeof buf);
    for (int line = 0; line < LINES; line++) {
        long base = line * ROW;
        if (base >= n)
            break;
        char text[96];
        int o = snprintf(text, sizeof text, "%08lx  ", offset + base);
        for (int i = 0; i < ROW; i++) {
            if (base + i < n)
                o += snprintf(text + o, sizeof text - (size_t)o, "%02x ", buf[base + i]);
            else
                o += snprintf(text + o, sizeof text - (size_t)o, "   ");
            if (i == 7)
                o += snprintf(text + o, sizeof text - (size_t)o, " ");
        }
        o += snprintf(text + o, sizeof text - (size_t)o, " |");
        for (int i = 0; i < ROW && base + i < n; i++) {
            unsigned char c = buf[base + i];
            text[o++] = c >= 32 && c < 127 ? (char)c : '.';
        }
        text[o++] = '|';
        text[o] = '\0';
        painter_text_font(p, f, 4, 2 + line * 16, text, 0x00000000, 0xffffffffu);
    }
    return 1;
}

static int on_key(struct widget *w, void *args, void *arg)
{
    struct sig_key *k = args;
    switch (k->code) {
    case KEY_UP: go_to(offset - ROW); return 1;
    case KEY_DOWN: go_to(offset + ROW); return 1;
    case KEY_PAGEUP: go_to(offset - LINES * ROW); return 1;
    case KEY_PAGEDOWN: go_to(offset + LINES * ROW); return 1;
    case KEY_HOME: go_to(0); return 1;
    case KEY_END: go_to(size); return 1;
    }
    return 0;
}

static int on_wheel(struct widget *w, void *args, void *arg)
{
    go_to(offset + 3 * ROW * ((struct sig_click *)args)->button);
    return 1;
}

static int on_scroll(struct widget *w, void *args, void *arg) { go_to((long)w->value * ROW); return 0; }

static int on_offset(struct widget *w, void *args, void *arg)
{
    go_to(strtol(widget_text(w), NULL, 16));
    return 1;
}

static int open_path(const char *name)
{
    int nfd = open(name, O_RDONLY);
    if (nfd < 0)
        return -1;
    if (fd >= 0)
        close(fd);
    fd = nfd;
    strlcpy(path, name, sizeof path);
    size = file_size();
    char s[300];
    snprintf(s, sizeof s, "%s: %ld bytes", path, size);
    widget_set_text(status, s);
    go_to(0);
    return 0;
}

static int on_open(struct widget *w, void *args, void *arg)
{
    char name[256];
    strlcpy(name, path, sizeof name);
    if (app_prompt(app, "Open", "File or device:", name, sizeof name) && open_path(name) < 0) {
        const char *const buttons[] = { "OK" };
        app_dialog(app, "Error", "The file cannot be opened.", buttons, 1);
    }
    return 1;
}

int main(int argc, char **argv)
{
    app = app_create();
    if (!app)
        return 1;
    struct widget *win = app_window(app, 660, 440, "hexview");
    if (!win)
        return 1;
    struct widget *tools = toolbar_new(win);
    widget_connect(button_new(tools, "Open..."), "clicked", on_open, NULL);
    label_new(tools, "Offset:");
    offset_field = textfield_new(tools, "0");
    widget_connect(offset_field, "activate", on_offset, NULL);
    struct widget *row = box_new(win, 0);
    widget_set_stretch(row, 1, 1);
    canvas = canvas_new(row);
    widget_connect(canvas, "paint", on_paint, NULL);
    widget_connect(canvas, "key", on_key, NULL);
    widget_connect(canvas, "wheel", on_wheel, NULL);
    bar = scrollbar_new(row, 1);
    widget_connect(bar, "scrolled", on_scroll, NULL);
    struct widget *sb = statusbar_new(win);
    status = statusbar_add(sb, 1);
    if (open_path(argc > 1 ? argv[1] : "/dev/vda") < 0)
        widget_set_text(status, "no file");
    widget_focus(canvas);
    app_run(app);
    app_destroy(app);
    return 0;
}
