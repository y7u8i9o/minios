/* unicode: a Unicode code point browser.
 *
 * The grid draws each code point with a chosen font and marks the ones
 * that font cannot draw. Coverage is asked of the font itself through
 * font_glyph_index, which returns glyph 0 when a code point is absent,
 * rather than guessed from the code point value.
 *
 * No fallback font is installed on purpose. A character map has to show
 * the repertoire of the selected font alone, and a fallback would fill
 * the gaps with another font's glyphs and misreport coverage.
 *
 * The default grid addresses code points directly and asks the font only
 * about the cells it is about to draw, so it opens without any scanning.
 * The "covered only" grid needs to map a cell index to the nth code
 * point the font covers; it classifies the code space lazily, extending
 * a table of contiguous runs only as far as the current view and
 * selection require.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gui/app.h>
#include <gui/utf8.h>
#include <font/font.h>
#include "unicode_blocks.h"

#define COLS        16
#define CELL_W      46
#define CELL_H      46
#define INFO_H      104
#define GRID_PX     32          /* multiple of 16 so Unifont's bitmap grid */
#define PREVIEW_PX  64          /* lands on whole pixels */
#define UNICODE_MAX 0x10ffffu
#define CODE_SPACE  (UNICODE_MAX + 1)

/* Fonts offered in the toolbar. Unifont is first because it is the only
 * one here with pan-Unicode coverage; the others are the UI and document
 * fonts, listed so their repertoires can be compared. */
static const struct {
    const char *label;
    const char *path;
} font_choices[] = {
    { "Unifont",     "/usr/share/fonts/unifont.otf" },
    { "DejaVu Sans", "/usr/share/fonts/DejaVuSans.ttf" },
    { "DejaVu Mono", "/usr/share/fonts/DejaVuSansMono.ttf" },
    { "Noto Sans",   "/usr/share/fonts/NotoSans-Regular.ttf" },
    { "LM Roman",    "/usr/share/fonts/lmroman10-regular.otf" },
};
#define NFONTS ((int)(sizeof font_choices / sizeof font_choices[0]))

/* A maximal run of code points the current font covers. before is the
 * number of covered code points in all earlier runs, which turns index
 * lookups in "covered only" mode into a binary search. */
struct run {
    uint32_t first, last, before;
};

static struct app *app;
static struct widget *info, *grid, *bar, *field, *status, *count_label;
static struct font *grid_font, *preview_font;

static struct run *runs;
static int nruns, runs_cap;
static uint32_t covered_total;
static uint32_t scan_next;      /* every code point below this is classified */

static uint32_t codepoint = 0x41;
static int covered_only;
static int top_row;
static int font_choice;

/* ---------------------------------------------------------------- code points */

static int scalar(uint32_t cp)
{
    return cp <= UNICODE_MAX && !(cp >= 0xd800 && cp <= 0xdfff);
}

static const char *block_name(uint32_t cp)
{
    int lo = 0, hi = (int)(sizeof unicode_blocks / sizeof unicode_blocks[0]) - 2;
    if (cp > UNICODE_MAX)
        return "Unassigned";
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (unicode_blocks[mid].first <= cp)
            lo = mid;
        else
            hi = mid - 1;
    }
    return unicode_blocks[lo].name;
}

/* UTF-8 for cp, empty for anything not encodable. Returns the length. */
static int encode(uint32_t cp, char out[5])
{
    if (!scalar(cp)) {
        out[0] = '\0';
        return 0;
    }
    int n = gui_utf8_encode(cp, out);
    out[n] = '\0';
    return n;
}

/* ------------------------------------------------------------------- coverage */

static int glyph_of(uint32_t cp)
{
    const struct ofont *of = grid_font ? grid_font->outline : NULL;
    return of && scalar(cp) ? font_glyph_index(of, cp) : 0;
}

static int scan_done(void)
{
    return scan_next >= CODE_SPACE;
}

static void coverage_reset(void)
{
    nruns = 0;
    covered_total = 0;
    scan_next = 0;
}

static void run_add(uint32_t first, uint32_t last)
{
    if (nruns == runs_cap) {
        int cap = runs_cap ? runs_cap * 2 : 256;
        struct run *grown = realloc(runs, (size_t)cap * sizeof *grown);
        if (!grown)
            return;
        runs = grown;
        runs_cap = cap;
    }
    runs[nruns].first = first;
    runs[nruns].last = last;
    runs[nruns].before = covered_total;
    nruns++;
    covered_total += last - first + 1;
}

/* Classify code points until want of them are known to be covered and
 * every code point up to and including through has been examined. Both
 * limits are what the caller needs right now, so the cost is paid in
 * proportion to how far the user has actually browsed. */
static void coverage_extend(uint32_t want, uint32_t through)
{
    if (!grid_font || !grid_font->outline)
        return;
    while (scan_next < CODE_SPACE && (covered_total < want || scan_next <= through)) {
        uint32_t cp = scan_next++;
        if (!glyph_of(cp))
            continue;
        if (nruns && runs[nruns - 1].last + 1 == cp) {
            runs[nruns - 1].last = cp;
            covered_total++;
        } else {
            run_add(cp, cp);
        }
    }
}

/* The index'th covered code point among those classified so far. */
static uint32_t covered_at(uint32_t index)
{
    if (!nruns)
        return 0;
    if (index >= covered_total)
        index = covered_total - 1;
    int lo = 0, hi = nruns - 1;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (runs[mid].before <= index)
            lo = mid;
        else
            hi = mid - 1;
    }
    return runs[lo].first + (index - runs[lo].before);
}

/* Index of cp among the covered code points, or of the nearest covered
 * one when cp itself is absent. */
static uint32_t covered_index(uint32_t cp)
{
    if (!nruns)
        return 0;
    int lo = 0, hi = nruns - 1;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (runs[mid].last < cp)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (cp < runs[lo].first)
        return runs[lo].before;
    if (cp > runs[lo].last)
        return covered_total ? covered_total - 1 : 0;
    return runs[lo].before + (cp - runs[lo].first);
}

/* --------------------------------------------------------------- grid model */

static int visible_rows(void)
{
    int rows = grid && grid->h > 0 ? grid->h / CELL_H : 1;
    return rows > 0 ? rows : 1;
}

static uint32_t cell_count(void)
{
    return covered_only ? covered_total : CODE_SPACE;
}

static uint32_t cell_codepoint(uint32_t index)
{
    if (!covered_only)
        return index;
    coverage_extend(index + 1, 0);
    return covered_at(index);
}

static uint32_t codepoint_cell(uint32_t cp)
{
    if (!covered_only)
        return cp;
    coverage_extend(0, cp);
    return covered_index(cp);
}

/* While the code space is only partly classified the covered grid includes
 * one extra screen of rows, so that scrolling past the end reaches
 * unclassified code points and extends the table. */
static int total_rows(void)
{
    int rows = (int)((cell_count() + COLS - 1) / COLS);
    if (covered_only && !scan_done())
        rows += visible_rows();
    return rows;
}

static void clamp_top(void)
{
    int max = total_rows() - visible_rows();
    if (top_row > max)
        top_row = max;
    if (top_row < 0)
        top_row = 0;
}

static void scroll_to_selection(void)
{
    if (!cell_count())
        return;
    int row = (int)(codepoint_cell(codepoint) / COLS);
    if (row < top_row)
        top_row = row;
    else if (row >= top_row + visible_rows())
        top_row = row - visible_rows() + 1;
    clamp_top();
}

/* ------------------------------------------------------------------- status */

static void status_update(void)
{
    char utf8[5], bytes[32] = "";
    int n = encode(codepoint, utf8), at = 0;
    for (int i = 0; i < n; i++)
        at += snprintf(bytes + at, sizeof bytes - (size_t)at, "%s%02X",
                       i ? " " : "", (unsigned char)utf8[i]);
    if (!n)
        strlcpy(bytes, "none", sizeof bytes);

    char text[192];
    int glyph = glyph_of(codepoint);
    snprintf(text, sizeof text, "U+%04X  %s  UTF-8 %s  %s", codepoint,
             block_name(codepoint), bytes,
             glyph ? "in this font" : "not in this font");
    widget_set_text(status, text);

    /* The glyph count comes from the font header, so the status bar
     * costs nothing; the covered count is shown only once the lazy scan
     * has finished on its own. */
    char summary[64];
    const struct ofont *of = grid_font ? grid_font->outline : NULL;
    if (covered_only && scan_done())
        snprintf(summary, sizeof summary, "%u code points", covered_total);
    else
        snprintf(summary, sizeof summary, "%d glyphs", of ? font_glyph_count(of) : 0);
    widget_set_text(count_label, summary);
}

static void refresh(void)
{
    if (covered_only)
        coverage_extend((uint32_t)(top_row + visible_rows() + 1) * COLS, 0);
    clamp_top();
    scrollbar_set(bar, top_row, total_rows(), visible_rows());
    status_update();
    widget_invalidate(grid);
    widget_invalidate(info);
}

static void select_codepoint(uint32_t cp)
{
    if (cp > UNICODE_MAX)
        cp = UNICODE_MAX;
    if (!scalar(cp))
        cp = 0xe000;
    /* The covered grid can only address covered code points, so snap to
     * the nearest one. */
    if (covered_only && !glyph_of(cp)) {
        coverage_extend(0, cp);
        if (covered_total)
            cp = covered_at(covered_index(cp));
    }
    codepoint = cp;

    char hex[16];
    snprintf(hex, sizeof hex, "%04X", codepoint);
    widget_set_text(field, hex);
    scroll_to_selection();
    refresh();
}

/* -------------------------------------------------------------------- fonts */

static void font_apply(int choice)
{
    if (choice < 0 || choice >= NFONTS)
        return;
    struct font *g = gfx_font_open_ttf(font_choices[choice].path, GRID_PX);
    if (!g) {
        char msg[128];
        snprintf(msg, sizeof msg, "%s cannot be opened", font_choices[choice].path);
        widget_set_text(status, msg);
        return;
    }
    gfx_font_free(grid_font);
    gfx_font_free(preview_font);
    grid_font = g;
    preview_font = gfx_font_open_ttf(font_choices[choice].path, PREVIEW_PX);
    font_choice = choice;
    coverage_reset();
    select_codepoint(codepoint);
}

/* ------------------------------------------------------------------ painting */

static void draw_glyph(struct painter *p, const struct font *f, uint32_t cp,
                       int x, int y, int w, uint32_t color)
{
    char text[5];
    if (!f || !encode(cp, text))
        return;
    int tw = painter_text_width_font(p, f, text, -1);
    painter_text_font(p, f, x + (w - tw) / 2, y, text, color, 0xffffffffu);
}

static int on_paint_info(struct widget *w, void *args, void *arg)
{
    struct painter *p = ((struct sig_paint *)args)->p;
    const struct theme *t = widget_theme(w);
    painter_fill(p, 0, 0, w->w, w->h, t->color[TC_FIELD]);
    painter_frame(p, 0, 0, w->w, w->h, t->color[TC_BORDER]);

    int glyph = glyph_of(codepoint);
    int box = w->h - 16;
    painter_fill(p, 8, 8, box, box, t->color[TC_WINDOW]);
    painter_frame(p, 8, 8, box, box, t->color[TC_BORDER]);
    if (glyph)
        draw_glyph(p, preview_font, codepoint, 8, 12, box, t->color[TC_TEXT]);
    else
        painter_text(p, 8 + box / 2 - 12, 8 + box / 2 - 6, "n/a",
                     t->color[TC_TEXT_DISABLED]);

    char line[192], utf8[5];
    int x = box + 24;
    snprintf(line, sizeof line, "U+%04X", codepoint);
    painter_text(p, x, 12, line, t->color[TC_TEXT]);
    painter_text(p, x, 32, block_name(codepoint), t->color[TC_TEXT]);

    int n = encode(codepoint, utf8);
    int at = snprintf(line, sizeof line, "UTF-8 ");
    for (int i = 0; i < n; i++)
        at += snprintf(line + at, sizeof line - (size_t)at, "%s%02X",
                       i ? " " : "", (unsigned char)utf8[i]);
    if (!n)
        snprintf(line + at, sizeof line - (size_t)at, "not encodable");
    painter_text(p, x, 52, line, t->color[TC_TEXT_DISABLED]);

    if (glyph)
        snprintf(line, sizeof line, "%s: glyph %d",
                 font_choices[font_choice].label, glyph);
    else
        snprintf(line, sizeof line, "%s: no glyph for this code point",
                 font_choices[font_choice].label);
    painter_text(p, x, 72, line, t->color[TC_TEXT_DISABLED]);
    return 1;
}

static int on_paint_grid(struct widget *w, void *args, void *arg)
{
    struct painter *p = ((struct sig_paint *)args)->p;
    const struct theme *t = widget_theme(w);
    painter_fill(p, 0, 0, w->w, w->h, t->color[TC_FIELD]);

    uint32_t cells = cell_count();
    int rows = visible_rows();
    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < COLS; col++) {
            uint32_t index = (uint32_t)(top_row + row) * COLS + (uint32_t)col;
            if (index >= cells)
                return 1;
            uint32_t cp = cell_codepoint(index);
            int x = col * CELL_W, y = row * CELL_H;
            int selected = cp == codepoint;
            int glyph = glyph_of(cp);

            uint32_t fill = selected ? t->color[TC_SELECTION]
                          : glyph    ? t->color[TC_FIELD]
                                     : t->color[TC_WINDOW];
            painter_fill(p, x, y, CELL_W - 1, CELL_H - 1, fill);
            painter_frame(p, x, y, CELL_W - 1, CELL_H - 1, t->color[TC_BORDER]);

            uint32_t ink = selected ? t->color[TC_SELECTION_TEXT] : t->color[TC_TEXT];
            if (glyph)
                draw_glyph(p, grid_font, cp, x, y + 2, CELL_W - 1, ink);

            char hex[12];
            snprintf(hex, sizeof hex, "%03X", cp & 0xfff);
            painter_text(p, x + 3, y + CELL_H - 14, hex,
                         selected ? t->color[TC_SELECTION_TEXT]
                                  : t->color[TC_TEXT_DISABLED]);
        }
    }
    return 1;
}

/* -------------------------------------------------------------------- events */

static int on_press(struct widget *w, void *args, void *arg)
{
    struct sig_click *c = args;
    if (!(c->button & 1))
        return 1;
    int col = c->x / CELL_W, row = c->y / CELL_H;
    if (col < 0 || col >= COLS || row < 0 || row >= visible_rows())
        return 1;
    uint32_t index = (uint32_t)(top_row + row) * COLS + (uint32_t)col;
    if (index < cell_count())
        select_codepoint(cell_codepoint(index));
    widget_focus(grid);
    return 1;
}

/* Move the selection by delta cells within the current grid mode. */
static void move(int delta)
{
    uint32_t cells = cell_count();
    if (!cells)
        return;
    long index = (long)codepoint_cell(codepoint) + delta;
    if (index < 0)
        index = 0;
    if (index >= (long)cells)
        index = (long)cells - 1;
    select_codepoint(cell_codepoint((uint32_t)index));
}

static int on_key(struct widget *w, void *args, void *arg)
{
    struct sig_key *k = args;
    int page = visible_rows() * COLS;
    switch (k->code) {
    case KEY_KP4: case KEY_LEFT: move(-1); return 1;
    case KEY_KP6: case KEY_RIGHT: move(1); return 1;
    case KEY_KP8: case KEY_UP: move(-COLS); return 1;
    case KEY_KP2: case KEY_DOWN: move(COLS); return 1;
    case KEY_KP9: case KEY_PAGEUP: move(-page); return 1;
    case KEY_KP3: case KEY_PAGEDOWN: move(page); return 1;
    case KEY_HOME: select_codepoint(0); return 1;
    case KEY_END: select_codepoint(UNICODE_MAX); return 1;
    }
    return 0;
}

static int on_wheel(struct widget *w, void *args, void *arg)
{
    top_row += 3 * ((struct sig_click *)args)->button;
    refresh();
    return 1;
}

static int on_scroll(struct widget *w, void *args, void *arg)
{
    top_row = w->value;
    refresh();
    return 0;
}

static int on_resize(struct widget *w, void *args, void *arg)
{
    refresh();
    return 0;
}

static int on_goto(struct widget *w, void *args, void *arg)
{
    const char *s = widget_text(field);
    char *end;
    unsigned long cp = strtoul(s, &end, 16);
    if (!s[0] || end == s || *end || cp > UNICODE_MAX) {
        widget_set_text(status, "Enter a hexadecimal code point from 0 to 10FFFF");
        return 1;
    }
    select_codepoint((uint32_t)cp);
    widget_focus(grid);
    return 1;
}

static int on_page(struct widget *w, void *args, void *arg)
{
    move((int)(long)arg * visible_rows() * COLS);
    widget_focus(grid);
    return 1;
}

static int on_covered(struct widget *w, void *args, void *arg)
{
    covered_only = w->value;
    select_codepoint(codepoint);
    widget_focus(grid);
    return 1;
}

static int on_font(struct widget *w, void *args, void *arg)
{
    font_apply(w->value);
    widget_focus(grid);
    return 1;
}

int main(void)
{
    app = app_create();
    if (!app)
        return 1;
    struct widget *win = app_window(app, 800, 660, "Unicode Viewer");
    if (!win)
        return 1;

    struct widget *tools = toolbar_new(win);
    label_new(tools, "Code point:");
    field = textfield_new(tools, "0041");
    widget_set_min(field, 80, 0);
    widget_set_max(field, 110, 0);
    widget_connect(field, "activate", on_goto, NULL);
    widget_connect(button_new(tools, "Go"), "clicked", on_goto, NULL);
    widget_connect(button_new(tools, "< Page"), "clicked", on_page, (void *)(long)-1);
    widget_connect(button_new(tools, "Page >"), "clicked", on_page, (void *)(long)1);
    struct widget *only = checkbox_new(tools, "Covered only");
    widget_connect(only, "toggled", on_covered, NULL);
    label_new(tools, "Font:");
    struct widget *combo = combobox_new(tools);
    for (int i = 0; i < NFONTS; i++)
        combobox_add(combo, font_choices[i].label);
    combobox_select(combo, 0);
    widget_connect(combo, "changed", on_font, NULL);

    info = canvas_new(win);
    /* A fixed preferred height and no vertical stretch. box_layout
     * advances by the size it allots while place() clamps the widget to
     * max_h, so setting a max shorter than the allotment would leave a
     * hole between this panel and the grid. */
    widget_set_min(info, COLS * CELL_W, INFO_H);
    widget_set_hint(info, COLS * CELL_W, INFO_H);
    widget_set_stretch(info, 1, 0);
    widget_connect(info, "paint", on_paint_info, NULL);

    struct widget *row = box_new(win, 0);
    widget_set_stretch(row, 1, 1);
    grid = canvas_new(row);
    grid->focusable = 1;
    widget_set_min(grid, COLS * CELL_W, CELL_H);
    widget_set_stretch(grid, 1, 1);
    widget_connect(grid, "paint", on_paint_grid, NULL);
    widget_connect(grid, "press", on_press, NULL);
    widget_connect(grid, "key", on_key, NULL);
    widget_connect(grid, "wheel", on_wheel, NULL);
    widget_connect(grid, "resize", on_resize, NULL);
    bar = scrollbar_new(row, 1);
    widget_set_stretch(bar, 0, 1);
    widget_connect(bar, "scrolled", on_scroll, NULL);

    struct widget *sb = statusbar_new(win);
    status = statusbar_add(sb, 1);
    count_label = statusbar_add(sb, 0);

    font_apply(0);
    widget_focus(grid);
    app_run(app);

    gfx_font_free(grid_font);
    gfx_font_free(preview_font);
    free(runs);
    app_destroy(app);
    return 0;
}
