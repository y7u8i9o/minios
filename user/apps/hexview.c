/* hexview: a hexadecimal viewer for files and devices.
 *
 *   hexview [PATH]
 *
 * The dump shows sixteen bytes per line with their offset and their
 * characters, in as many lines as the window has room for. A cursor
 * selects a byte in the hexadecimal or the character column, and Shift
 * with a movement or a drag extends a selection. The inspector shows the
 * bytes at the cursor as integers and floating point numbers of every
 * size, in little or big endian order. Find searches for text or for a
 * byte sequence from the cursor and continues at the start of the file.
 * Copy puts the selection on the clipboard as hexadecimal digits or as
 * characters, after the column of the cursor. Without PATH the first disk,
 * /dev/vda, is shown. The file is opened read only.
 *
 * Keys: the arrows, Page Up and Page Down move the cursor; Home and End
 * go to the start and the end of the line, with Ctrl to the start and
 * the end of the file; Tab changes the column; Ctrl+F finds text, F3
 * finds the next match, Ctrl+G goes to an offset, Ctrl+C copies, Ctrl+O
 * opens, and Ctrl with + and - changes the text size. */
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <gui/app.h>
#include <minios/input.h>

#define ROW 16
#define FONT_PATH "/etc/fonts/DejaVuSansMono.ttf"
#define DEFAULT_PX 13
#define FIND_MAX 256
#define CHUNK 65536
#define COPY_MAX 16384          /* bytes; four times that in clipboard text */

static struct app *app;
static struct widget *win, *canvas, *bar, *offset_field, *inspector, *endian_box;
static struct widget *st_path, *st_size, *st_cursor, *st_sel;
enum { V_OFFSET, V_BINARY, V_I8, V_U8, V_I16, V_U16, V_I32, V_U32, V_I64, V_U64, V_F32, V_F64, NVALUES };
static const char *const value_names[NVALUES] = {
    "Offset", "Binary", "Int8", "UInt8", "Int16", "UInt16", "Int32", "UInt32", "Int64", "UInt64", "Float32", "Float64",
};
static struct widget *values[NVALUES];

static int fd = -1;
static char path[PATH_MAX];
static long size;               /* bytes */
static long top;                /* offset of the first line shown, a multiple of ROW */
static long cursor, anchor;     /* the selection is from anchor to cursor, both included */
static int column;              /* 0 hexadecimal, 1 characters */
static int big_endian;
static int lines = 1;           /* lines the canvas shows */

static struct font *font;
static int font_px = DEFAULT_PX, cw = 8, ch = 16;

static uint8_t pattern[FIND_MAX];
static int pattern_len;
static char find_text[FIND_MAX * 3];   /* the last search, for the messages */
static char last_text[FIND_MAX], last_bytes[FIND_MAX * 3];   /* the previous entry of each prompt */

/* ---- font ---- */

static void load_font(int px)
{
    struct font *f = gfx_font_open_ttf(FONT_PATH, px);
    if (!f && font)
        return;
    if (font)
        gfx_font_free(font);
    font = f;
    font_px = px;
    const struct font *t = f ? f : gfx_font_builtin();
    cw = t->advance['0'];
    ch = t->height + 2;
}

static const struct font *text_font(void) { return font ? font : gfx_font_builtin(); }

/* ---- reading ---- */

static long read_at(long off, uint8_t *buf, long n)
{
    if (fd < 0 || off < 0 || off >= size)
        return 0;
    if (n > size - off)
        n = size - off;
    if (lseek(fd, off, SEEK_SET) < 0)
        return 0;
    long done = 0;
    while (done < n) {
        long r = read(fd, buf + done, (size_t)(n - done));
        if (r <= 0)
            break;
        done += r;
    }
    return done;
}

/* ---- geometry ---- */

static int offset_digits(void)
{
    int d = 8;
    while (d < 16 && size > 0 && ((unsigned long)(size - 1) >> (d * 4)) != 0)
        d++;
    return d;
}

static int hex_x(void) { return 6 + (offset_digits() + 2) * cw; }
static int byte_x(int i) { return hex_x() + (i * 3 + (i >= 8)) * cw; }
static int text_x(void) { return hex_x() + (ROW * 3 + 2) * cw; }
static int dump_width(void) { return text_x() + ROW * cw + 6; }

static long sel_start(void) { return anchor < cursor ? anchor : cursor; }
static long sel_end(void) { return anchor < cursor ? cursor : anchor; }

/* ---- state shown outside the canvas ---- */

static void update_scroll(void)
{
    long total = (size + ROW - 1) / ROW;
    scrollbar_set(bar, (int)(top / ROW), (int)total, lines);
}

static void update_inspector(void)
{
    uint8_t b[8];
    long n = read_at(cursor, b, 8);
    char text[80];
    snprintf(text, sizeof text, "0x%lx (%ld)", cursor, cursor);
    widget_set_text(values[V_OFFSET], size ? text : "");
    for (int k = V_BINARY; k < NVALUES; k++) {
        int need = k <= V_U8 ? 1 : k <= V_U16 ? 2 : k <= V_U32 || k == V_F32 ? 4 : 8;
        if (n < need) {
            widget_set_text(values[k], "");
            continue;
        }
        uint64_t v = 0;
        for (int i = 0; i < need; i++)
            v |= (uint64_t)b[big_endian ? need - 1 - i : i] << (8 * i);
        switch (k) {
        case V_BINARY:
            for (int i = 0; i < 8; i++)
                text[i] = (char)('0' + ((b[0] >> (7 - i)) & 1));
            text[8] = '\0';
            break;
        case V_I8: snprintf(text, sizeof text, "%d", (int8_t)v); break;
        case V_U8: snprintf(text, sizeof text, "%u", (unsigned)v); break;
        case V_I16: snprintf(text, sizeof text, "%d", (int16_t)v); break;
        case V_U16: snprintf(text, sizeof text, "%u", (unsigned)v); break;
        case V_I32: snprintf(text, sizeof text, "%d", (int32_t)v); break;
        case V_U32: snprintf(text, sizeof text, "%u", (uint32_t)v); break;
        case V_I64: snprintf(text, sizeof text, "%lld", (long long)(int64_t)v); break;
        case V_U64: snprintf(text, sizeof text, "%llu", (unsigned long long)v); break;
        case V_F32: {
            uint32_t u = (uint32_t)v;
            float f;
            memcpy(&f, &u, 4);
            snprintf(text, sizeof text, "%g", (double)f);
            break;
        }
        default: {
            double d;
            memcpy(&d, &v, 8);
            snprintf(text, sizeof text, "%g", d);
            break;
        }
        }
        widget_set_text(values[k], text);
    }
}

static void update_status(void)
{
    char text[64];
    snprintf(text, sizeof text, "%ld bytes", size);
    widget_set_text(st_size, fd >= 0 ? text : "");
    snprintf(text, sizeof text, "0x%lx", cursor);
    widget_set_text(st_cursor, size ? text : "");
    if (sel_end() > sel_start()) {
        snprintf(text, sizeof text, "%ld selected", sel_end() - sel_start() + 1);
        widget_set_text(st_sel, text);
    } else {
        widget_set_text(st_sel, "");
    }
    snprintf(text, sizeof text, "%lx", cursor);
    widget_set_text(offset_field, text);
    update_inspector();
}

static void changed(void)
{
    update_scroll();
    update_status();
    widget_invalidate(canvas);
}

/* Move the cursor, extending the selection or not, and scroll it into
 * view. */
static void move_to(long off, int extend)
{
    if (size == 0)
        off = 0;
    else if (off >= size)
        off = size - 1;
    if (off < 0)
        off = 0;
    cursor = off;
    if (!extend)
        anchor = off;
    long line = cursor / ROW * ROW;
    if (line < top)
        top = line;
    else if (line >= top + (long)lines * ROW)
        top = line - (long)(lines - 1) * ROW;
    changed();
}

static void scroll_to(long line_offset)
{
    long last = (size + ROW - 1) / ROW - lines;
    long l = line_offset / ROW;
    if (l > last)
        l = last;
    if (l < 0)
        l = 0;
    top = l * ROW;
    update_scroll();
    widget_invalidate(canvas);
}

/* ---- painting ---- */

static int on_paint(struct widget *w, void *args, void *arg)
{
    struct painter *p = ((struct sig_paint *)args)->p;
    const struct theme *t = p->theme;
    const struct font *f = text_font();
    painter_fill(p, 0, 0, w->w, w->h, t->color[TC_FIELD]);
    int n_lines = (w->h - 4) / ch > 0 ? (w->h - 4) / ch : 1;
    if (n_lines != lines) {
        lines = n_lines;
        update_scroll();
    }
    long want = (long)lines * ROW;
    uint8_t *buf = malloc((size_t)want);
    long n = buf ? read_at(top, buf, want) : 0;
    long s0 = sel_start(), s1 = sel_end();
    uint32_t text_c = t->color[TC_TEXT], dim = 0x00a0a0a0, off_c = 0x00808080;
    uint32_t sel_bg = t->color[TC_SELECTION], sel_fg = t->color[TC_SELECTION_TEXT], accent = t->color[TC_ACCENT];
    /* A line between the offsets and the bytes, and between the bytes and
     * the characters. */
    painter_fill(p, hex_x() - cw, 0, 1, w->h, t->color[TC_BORDER]);
    painter_fill(p, text_x() - cw, 0, 1, w->h, t->color[TC_BORDER]);
    char s[24];
    for (int line = 0; line < lines; line++) {
        long base = (long)line * ROW;
        if (base >= n)
            break;
        int y = 2 + line * ch;
        snprintf(s, sizeof s, "%0*lx", offset_digits(), top + base);
        painter_text_font(p, f, 6, y, s, off_c, 0xffffffffu);
        for (int i = 0; i < ROW && base + i < n; i++) {
            long off = top + base + i;
            uint8_t b = buf[base + i];
            int selected = off >= s0 && off <= s1;
            int bx = byte_x(i), tx = text_x() + i * cw;
            if (selected) {
                /* The gap after a selected byte is filled when the next
                 * byte of the line is selected too. */
                int wide = i < ROW - 1 && off < s1 ? (i == 7 ? 4 : 3) : 2;
                painter_fill(p, bx, y, wide * cw, ch, sel_bg);
                painter_fill(p, tx, y, cw, ch, sel_bg);
            }
            snprintf(s, sizeof s, "%02x", b);
            painter_text_font(p, f, bx, y, s, selected ? sel_fg : b == 0 ? dim : text_c, 0xffffffffu);
            int printable = b >= 32 && b < 127;
            s[0] = printable ? (char)b : '.';
            s[1] = '\0';
            painter_text_font(p, f, tx, y, s, selected ? sel_fg : printable ? text_c : dim, 0xffffffffu);
            if (off == cursor) {
                painter_frame(p, bx - 1, y, 2 * cw + 2, ch, column == 0 ? accent : t->color[TC_BORDER]);
                painter_frame(p, tx, y, cw, ch, column == 1 ? accent : t->color[TC_BORDER]);
            }
        }
    }
    free(buf);
    return 1;
}

/* ---- mouse and keys ---- */

/* The byte under a point of the canvas, and its column; -1 outside the
 * bytes. */
static long byte_at(int x, int y, int *col)
{
    int line = (y - 2) / ch;
    if (y < 2 || line >= lines)
        return -1;
    int i = -1;
    if (x >= hex_x() && x < text_x() - cw) {
        for (int k = 0; k < ROW; k++)
            if (x >= byte_x(k) && x < byte_x(k) + 3 * cw)
                i = k;
        *col = 0;
    } else if (x >= text_x() && x < text_x() + ROW * cw) {
        i = (x - text_x()) / cw;
        *col = 1;
    }
    if (i < 0)
        return -1;
    long off = top + (long)line * ROW + i;
    return off < size ? off : size - 1;
}

static int on_press(struct widget *w, void *args, void *arg)
{
    struct sig_click *c = args;
    widget_focus(canvas);
    int col = column;
    long off = byte_at(c->x, c->y, &col);
    if (!(c->button & 1) || off < 0)
        return 1;
    column = col;
    move_to(off, gui_modifiers() & WMOD_SHIFT);
    return 1;
}

static int on_motion(struct widget *w, void *args, void *arg)
{
    struct sig_click *c = args;
    if (!(c->button & 1))
        return 0;
    int col = column;
    if (c->y < 2) {
        move_to(cursor - ROW, 1);
        return 1;
    }
    if (c->y >= 2 + lines * ch) {
        move_to(cursor + ROW, 1);
        return 1;
    }
    long off = byte_at(c->x, c->y, &col);
    if (off >= 0 && off != cursor)
        move_to(off, 1);
    return 1;
}

static int on_wheel(struct widget *w, void *args, void *arg)
{
    scroll_to(top + 3L * ROW * ((struct sig_click *)args)->button);
    return 1;
}

static int on_scroll(struct widget *w, void *args, void *arg)
{
    top = (long)w->value * ROW;
    widget_invalidate(canvas);
    return 0;
}

static void find_next(void);

static int on_key(struct widget *w, void *args, void *arg)
{
    struct sig_key *k = args;
    int shift = (k->mods & WMOD_SHIFT) != 0, ctrl = (k->mods & WMOD_CTRL) != 0;
    long page = (long)(lines > 1 ? lines - 1 : 1) * ROW;
    switch (k->code) {
    case KEY_LEFT: move_to(cursor - 1, shift); return 1;
    case KEY_RIGHT: move_to(cursor + 1, shift); return 1;
    case KEY_UP: move_to(cursor - ROW, shift); return 1;
    case KEY_DOWN: move_to(cursor + ROW, shift); return 1;
    case KEY_PAGEUP: top = top >= page ? top - page : 0; move_to(cursor - page, shift); return 1;
    case KEY_PAGEDOWN: scroll_to(top + page); move_to(cursor + page, shift); return 1;
    case KEY_HOME: move_to(ctrl ? 0 : cursor / ROW * ROW, shift); return 1;
    case KEY_END: move_to(ctrl ? size - 1 : cursor / ROW * ROW + ROW - 1, shift); return 1;
    case KEY_TAB: column = !column; widget_invalidate(canvas); return 1;
    case KEY_F3: find_next(); return 1;
    }
    return 0;
}

/* ---- commands ---- */

static void message(const char *text)
{
    const char *const buttons[] = { "OK" };
    app_dialog(app, "Hex viewer", text, buttons, 1);
}

static int open_path(const char *name)
{
    int nfd = open(name, O_RDONLY);
    if (nfd < 0)
        return -errno;
    long end = lseek(nfd, 0, SEEK_END);
    if (fd >= 0)
        close(fd);
    fd = nfd;
    size = end > 0 ? end : 0;
    strlcpy(path, name, sizeof path);
    top = cursor = anchor = 0;
    widget_set_text(st_path, path);
    const char *base = strrchr(path, '/');
    gui_set_title(window_state_of(win)->win, base && base[1] ? base + 1 : path);
    changed();
    return 0;
}

static int on_open(struct widget *w, void *args, void *arg)
{
    char name[PATH_MAX];
    strlcpy(name, path, sizeof name);
    if (!app_choose_file(app, FILE_CHOOSER_OPEN, "Open file or device", NULL, 0, name, sizeof name))
        return 1;
    int err = open_path(name);
    if (err < 0) {
        char text[PATH_MAX + 64];
        snprintf(text, sizeof text, "%s cannot be opened: %s", name, strerror(-err));
        message(text);
    }
    return 1;
}

/* The first match of the pattern at or after from, continuing at the start
 * of the file; -1 when there is none. Chunks overlap by the pattern
 * length less one byte, so that a match across two chunks is found. */
static long search(long from)
{
    if (pattern_len == 0 || size < pattern_len)
        return -1;
    uint8_t *buf = malloc(CHUNK + FIND_MAX);
    if (!buf)
        return -1;
    long found = -1;
    for (int pass = 0; pass < 2 && found < 0; pass++) {
        long start = pass == 0 ? from : 0, limit = pass == 0 ? size : from + pattern_len - 1;
        if (limit > size)
            limit = size;
        for (long off = start; off + pattern_len <= limit && found < 0; off += CHUNK) {
            long n = read_at(off, buf, CHUNK + pattern_len - 1);
            if (off + n > limit)
                n = limit - off;
            for (long i = 0; i + pattern_len <= n; i++)
                if (buf[i] == pattern[0] && memcmp(buf + i, pattern, (size_t)pattern_len) == 0) {
                    found = off + i;
                    break;
                }
        }
    }
    free(buf);
    return found;
}

static void find_from(long from)
{
    long at = search(from);
    if (at < 0) {
        printf("hexview: %s not found\n", find_text);
        fflush(stdout);
        widget_set_text(st_sel, "not found");
        return;
    }
    printf("hexview: %s found at 0x%lx\n", find_text, at);
    fflush(stdout);
    /* The cursor goes to the first byte of the match, so that the
     * inspector shows the values that start there. */
    anchor = at + pattern_len - 1;
    move_to(at, 1);
}

static void find_next(void)
{
    if (pattern_len == 0)
        return;
    find_from(sel_start() + 1 < size ? sel_start() + 1 : 0);
}

/* Bytes written as pairs of hexadecimal digits, with or without spaces. */
static int parse_bytes(const char *text, uint8_t *out)
{
    int n = 0, half = -1;
    for (const char *s = text; *s; s++) {
        int v = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'a' && *s <= 'f' ? *s - 'a' + 10
              : *s >= 'A' && *s <= 'F' ? *s - 'A' + 10 : -1;
        if (v < 0) {
            if (*s == ' ' && half < 0)
                continue;
            return -1;
        }
        if (half < 0) {
            half = v;
        } else {
            if (n == FIND_MAX)
                return -1;
            out[n++] = (uint8_t)(half << 4 | v);
            half = -1;
        }
    }
    return half < 0 && n > 0 ? n : -1;
}

static int on_find(struct widget *w, void *args, void *arg)
{
    int bytes = arg != NULL;
    char text[sizeof find_text];
    strlcpy(text, bytes ? last_bytes : last_text, sizeof text);
    if (!app_prompt(app, bytes ? "Find bytes" : "Find text", bytes ? "Hexadecimal bytes:" : "Text:", text, sizeof text) || !text[0])
        return 1;
    if (bytes) {
        int n = parse_bytes(text, pattern);
        if (n < 0) {
            message("The bytes must be pairs of hexadecimal digits.");
            return 1;
        }
        pattern_len = n;
    } else {
        pattern_len = (int)strlen(text) < FIND_MAX ? (int)strlen(text) : FIND_MAX;
        memcpy(pattern, text, (size_t)pattern_len);
    }
    strlcpy(find_text, text, sizeof find_text);
    if (bytes)
        strlcpy(last_bytes, text, sizeof last_bytes);
    else
        strlcpy(last_text, text, sizeof last_text);
    find_from(sel_start() + 1 < size ? sel_start() + 1 : 0);
    return 1;
}

static int on_find_next(struct widget *w, void *args, void *arg) { find_next(); return 1; }

static int on_offset(struct widget *w, void *args, void *arg)
{
    const char *text = widget_text(w);
    char *end;
    long off = strtol(text, &end, 16);
    if (end == text || *end) {
        message("The offset must be a hexadecimal number.");
        return 1;
    }
    move_to(off, 0);
    widget_focus(canvas);
    return 1;
}

static int on_goto(struct widget *w, void *args, void *arg)
{
    widget_focus(offset_field);
    return 1;
}

static int on_copy(struct widget *w, void *args, void *arg)
{
    long n = sel_end() - sel_start() + 1;
    if (size == 0)
        return 1;
    if (n > COPY_MAX)
        n = COPY_MAX;
    uint8_t *b = malloc((size_t)n);
    char *text = malloc((size_t)n * 3 + 1);
    if (b && text) {
        n = read_at(sel_start(), b, n);
        size_t o = 0;
        for (long i = 0; i < n; i++) {
            if (column == 0)
                o += (size_t)snprintf(text + o, 4, i ? " %02x" : "%02x", b[i]);
            else
                text[o++] = b[i] >= 32 && b[i] < 127 ? (char)b[i] : '.';
        }
        text[o] = '\0';
        gui_clipboard_set(text, (int)o);
    }
    free(b);
    free(text);
    return 1;
}

static void set_font_px(int px)
{
    if (px < 8 || px > 32)
        return;
    load_font(px);
    widget_set_hint(canvas, dump_width(), 0);
    widget_relayout(win);
    changed();
}

static int on_larger(struct widget *w, void *args, void *arg) { set_font_px(font_px + 1); return 1; }
static int on_smaller(struct widget *w, void *args, void *arg) { set_font_px(font_px - 1); return 1; }
static int on_normal(struct widget *w, void *args, void *arg) { set_font_px(DEFAULT_PX); return 1; }

static int on_inspector(struct widget *w, void *args, void *arg)
{
    widget_set_visible(inspector, !inspector->visible);
    widget_relayout(win);
    return 1;
}

static int on_endian(struct widget *w, void *args, void *arg)
{
    big_endian = w->value;
    update_inspector();
    return 1;
}

static int on_quit(struct widget *w, void *args, void *arg) { app_quit(app, 0); return 1; }

static struct widget *item(struct widget *menu, const char *text, const char *icon, signal_fn fn, void *arg,
                           int key, int mods)
{
    struct widget *m = menu_add(menu, text, icon);
    widget_connect(m, "clicked", fn, arg);
    if (key)
        widget_set_accel(m, key, mods);
    return m;
}

int main(int argc, char **argv)
{
    app = app_create();
    if (!app)
        return 1;
    load_font(DEFAULT_PX);
    win = app_window(app, 940, 520, "Hex viewer");
    if (!win)
        return 1;
    struct widget *mb = menubar_new(win);
    struct widget *file = menu_new(mb, "File");
    item(file, "Open...", "open", on_open, NULL, KEY_O, WMOD_CTRL);
    menu_add_separator(file);
    item(file, "Quit", "quit", on_quit, NULL, 0, 0);
    struct widget *edit = menu_new(mb, "Edit");
    item(edit, "Copy", "copy", on_copy, NULL, KEY_C, WMOD_CTRL);
    menu_add_separator(edit);
    item(edit, "Find text...", "search", on_find, NULL, KEY_F, WMOD_CTRL);
    item(edit, "Find bytes...", NULL, on_find, (void *)1, KEY_B, WMOD_CTRL);
    item(edit, "Find next", NULL, on_find_next, NULL, 0, 0);
    item(edit, "Go to offset...", NULL, on_goto, NULL, KEY_G, WMOD_CTRL);
    struct widget *view = menu_new(mb, "View");
    item(view, "Larger text", "zoom-in", on_larger, NULL, KEY_EQUAL, WMOD_CTRL);
    item(view, "Smaller text", "zoom-out", on_smaller, NULL, KEY_MINUS, WMOD_CTRL);
    item(view, "Normal size", NULL, on_normal, NULL, KEY_0, WMOD_CTRL);
    menu_add_separator(view);
    item(view, "Inspector", NULL, on_inspector, NULL, KEY_I, WMOD_CTRL);

    struct widget *tools = toolbar_new(win);
    widget_connect(toolbar_add(tools, "open", "Open"), "clicked", on_open, NULL);
    widget_connect(toolbar_add(tools, "copy", "Copy"), "clicked", on_copy, NULL);
    widget_connect(toolbar_add(tools, "search", "Find text"), "clicked", on_find, NULL);
    label_new(tools, "Offset");
    offset_field = textfield_new(tools, "0");
    widget_set_hint(offset_field, 140, 0);
    widget_set_max(offset_field, 140, 0);
    widget_connect(offset_field, "activate", on_offset, NULL);

    struct widget *row = box_new(win, 0);
    widget_set_stretch(row, 1, 1);
    canvas = canvas_new(row);
    canvas->focusable = 1;
    widget_set_stretch(canvas, 0, 1);
    widget_set_hint(canvas, dump_width(), 0);
    widget_connect(canvas, "paint", on_paint, NULL);
    widget_connect(canvas, "press", on_press, NULL);
    widget_connect(canvas, "motion", on_motion, NULL);
    widget_connect(canvas, "wheel", on_wheel, NULL);
    widget_connect(canvas, "key", on_key, NULL);
    bar = scrollbar_new(row, 1);
    widget_connect(bar, "scrolled", on_scroll, NULL);
    inspector = grid_new(row);
    widget_set_hint(inspector, 260, 0);
    widget_set_stretch(inspector, 1, 0);
    widget_set_align(inspector, ALIGN_FILL, ALIGN_START);
    for (int i = 0; i < NVALUES; i++) {
        struct widget *l = label_new(inspector, value_names[i]);
        widget_set_grid(l, i, 0, 1, 1);
        widget_set_hint(l, 70, 0);
        values[i] = label_new(inspector, "");
        widget_set_grid(values[i], i, 1, 1, 1);
    }
    endian_box = checkbox_new(inspector, "Big endian");
    widget_set_grid(endian_box, NVALUES, 0, 1, 2);
    widget_connect(endian_box, "toggled", on_endian, NULL);

    struct widget *sb = statusbar_new(win);
    st_path = statusbar_add(sb, 1);
    st_size = statusbar_add(sb, 0);
    widget_set_min(st_size, 130, 0);
    st_cursor = statusbar_add(sb, 0);
    widget_set_min(st_cursor, 90, 0);
    st_sel = statusbar_add(sb, 0);
    widget_set_min(st_sel, 100, 0);

    const char *name = argc > 1 ? argv[1] : "/dev/vda";
    int err = open_path(name);
    if (err < 0) {
        fprintf(stderr, "hexview: cannot open %s: %s\n", name, strerror(-err));
        widget_set_text(st_path, "no file");
        changed();
    }
    widget_focus(canvas);
    app_run(app);
    app_destroy(app);
    return 0;
}
