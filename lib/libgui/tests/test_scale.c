/* K1 of docs/plan/widgets.md: the widgets at scale 2. The windows of the
 * fake client have 2 by 2 device pixels per logical pixel, and the theme
 * uses DejaVu Sans. The builtin font is exactly twice as wide at scale 2
 * and would hide measurement at the wrong scale. */
#include <gui/app.h>
#include <stdlib.h>
#include <string.h>
#include "check.h"
#include "events.h"
#include "fake.h"

struct app *app_create_detached(void);
void widget_measure(struct widget *w);

#define S 2

/* The device pixel at (x, y) of the window surface. */
static uint32_t pixel(struct widget *win, int x, int y)
{
    struct surface *s = &window_state_of(win)->win->surf;
    return s->pixels[(size_t)y * s->stride + x] & 0xffffff;
}

static void send_text(struct widget *win, const char *text)
{
    struct wmsg m = { .type = WM_TEXT, .window = window_state_of(win)->win->id };
    strlcpy(m.text, text, sizeof m.text);
    window_message(win, &m);
}

static void send_key(struct widget *win, int code, int ch)
{
    struct wmsg m = key_msg(win, code, ch, 0);
    window_message(win, &m);
}

/* The caret of a focused text field is the only part of the field in the
 * colour of the text on the row above the text. The function returns its
 * logical column in the window, or -1. */
static int caret_column(struct widget *win, struct widget *field, const struct painter *p)
{
    int fx, fy;
    widget_abs(field, &fx, &fy);
    int ty = (field->h - 2 - painter_text_height(p)) / 2;
    int row = (fy + 1 + ty - 1) * S;
    for (int x = fx * S; x < (fx + field->w) * S; x++)
        if (pixel(win, x, row) == p->theme->color[TC_TEXT])
            return x / S;
    return -1;
}

static void test_textfield(struct app *a, const struct painter *p)
{
    struct widget *win = app_window(a, 300, 120, "field");
    struct widget *field = textfield_new(win, "");
    widget_set_max(field, 120, 0);
    window_paint(win);
    widget_focus(field);
    int fx, fy;
    widget_abs(field, &fx, &fy);

    /* A long text scrolls. The caret then lies at the right end of the
     * field, 4 pixels inside the padding. */
    send_text(win, "The quick brown fox jumps over the lazy dog");
    window_paint(win);
    int caret = caret_column(win, field, p);
    CHECK(caret == fx + field->w - 5, "caret of a scrolled field at %d, expected %d", caret, fx + field->w - 5);

    /* A short text puts the caret at the painted width of the text. */
    widget_set_text(field, "Wide");
    textfield_select(field, -1, -1);
    window_paint(win);
    int expected = fx + 4 + painter_text_width(p, "Wide", -1);
    caret = caret_column(win, field, p);
    CHECK(caret == expected, "caret at %d, painted prefix width gives %d", caret, expected);

    /* A click at the boundary after "Wid" puts the cursor there. */
    textfield_select(field, -1, 0);
    click(win, fx + 4 + painter_text_width(p, "Wid", 3), fy + field->h / 2);
    type_text(win, "|");
    CHECK(strcmp(widget_text(field), "Wid|e") == 0, "click at a glyph boundary: \"%s\"", widget_text(field));
}

static int picked = -1;
static int on_pick(struct widget *w, void *args, void *arg)
{
    picked = (int)(long)arg;
    return 1;
}

static void test_widths(struct app *a, const struct painter *p)
{
    struct widget *win = app_window(a, 400, 200, "widths");
    struct widget *combo = combobox_new(win);
    combobox_add(combo, "Small");
    combobox_add(combo, "A much longer entry");
    struct widget *tabs = tabs_new(win);
    tabs_add(tabs, "First");
    tabs_add(tabs, "Second");
    tabs_add(tabs, "Third");
    window_paint(win);
    int tw = painter_text_width(p, "A much longer entry", -1);
    CHECK(combo->measured.pref_w == tw + 18 + 12, "combo box width %d, painted %d", combo->measured.pref_w, tw + 30);

    /* The boundary between the second and the third tab. */
    int tx, ty;
    widget_abs(tabs, &tx, &ty);
    int edge = painter_text_width(p, "First", -1) + 20 + painter_text_width(p, "Second", -1) + 20;
    click(win, tx + edge - 1, ty + 4);
    CHECK(tabs->value == 1, "a click left of the edge selects the second tab: %d", tabs->value);
    click(win, tx + edge, ty + 4);
    CHECK(tabs->value == 2, "a click at the edge selects the third tab: %d", tabs->value);
}

static void test_menu(struct app *a, const struct painter *p)
{
    struct widget *win = app_window(a, 300, 200, "menu");
    struct widget *m = popupmenu_new(win);
    struct widget *open = menu_add(m, "&Open", NULL);
    menu_add_separator(m);
    struct widget *off = menu_add(m, "Disabled", NULL);
    widget_set_enabled(off, 0);
    struct widget *close = menu_add(m, "Close", NULL);
    widget_connect(open, "clicked", on_pick, (void *)0);
    widget_connect(close, "clicked", on_pick, (void *)3);
    menu_popup(m, 10, 10);
    window_paint(win);
    int ow = painter_text_width(p, "Open", -1);
    CHECK(open->measured.pref_w == ow + 2 * 10 + 24, "menu item width %d, painted %d", open->measured.pref_w,
          ow + 44);

    /* The first item shows "Open" without the marker and underlines O. */
    struct widget *d = window_state_of(win)->popup;
    int dx, dy;
    widget_abs(d, &dx, &dy);
    int ih = open->measured.pref_h, th = painter_text_height(p), stray = 0;
    for (int y = (dy + 2) * S; y < (dy + ih) * S; y++)
        for (int x = (dx + 10 + ow + 1) * S; x < (dx + d->w - 2) * S; x++)
            stray += pixel(win, x, y) != p->theme->color[TC_FIELD];
    CHECK(stray == 0, "%d pixels right of the caption of \"&Open\"", stray);
    int base = dy + 1 + 4 + th - 1, ox = dx + 10 + painter_text_width(p, "O", 1) / 2;
    CHECK(pixel(win, ox * S, base * S) == p->theme->color[TC_TEXT], "the mnemonic is underlined: %06x",
          pixel(win, ox * S, base * S));

    /* Down twice skips the separator and the disabled item. */
    send_key(win, KEY_DOWN, 0);
    send_key(win, KEY_DOWN, 0);
    send_key(win, KEY_ENTER, '\n');
    CHECK(picked == 3, "Down skips the separator and the disabled item: item %d", picked);
}

static void test_scrollbar(struct app *a, const struct painter *p)
{
    struct widget *win = app_window(a, 200, 300, "scroll");
    struct widget *sb = scrollbar_new(win, 1);
    scrollbar_set(sb, 0, 100, 25);
    window_paint(win);
    int sx, sy, off, len;
    widget_abs(sb, &sx, &sy);
    CHECK(scrollbar_thumb(sb->h, 0, 100, 25, &off, &len) > 0, "the thumb can travel");
    int mid = (sx + sb->w / 2) * S, top = -1;
    for (int y = sy * S; y < (sy + sb->h) * S && top < 0; y++)
        if (pixel(win, mid, y) == p->theme->color[TC_THUMB])
            top = y;
    CHECK(top == (sy + off) * S, "the painted thumb starts at %d, the geometry gives %d", top, (sy + off) * S);
    click(win, sx + sb->w / 2, sy + off);
    CHECK(sb->value == 0, "a press on the top row of the thumb grabs it: value %d", sb->value);
    click(win, sx + sb->w / 2, sy + off + len);
    CHECK(sb->value == 25, "a press below the thumb pages: value %d", sb->value);
}

static int activated = -1;
static int on_activate(struct widget *w, void *args, void *arg)
{
    activated = ((struct sig_select *)args)->index;
    return 1;
}

static void test_buttons_and_lists(struct app *a, const struct painter *p)
{
    const struct theme *t = p->theme;
    struct widget *win = app_window(a, 300, 200, "buttons");
    struct widget *cb = colorbutton_new(win, 0xff0000, "Colour");
    struct widget *list = listview_new(win);
    listview_add(list, "one");
    listview_add(list, "two");
    listview_add(list, "three");
    widget_connect(list, "activate", on_activate, NULL);
    window_paint(win);

    int bx, by;
    widget_abs(cb, &bx, &by);
    struct wmsg down = mouse_msg(win, WMOUSE_DOWN, bx + cb->w / 2, by + cb->h / 2, 1);
    window_message(win, &down);
    window_paint(win);
    CHECK(pixel(win, (bx + cb->w - 4) * S, (by + cb->h / 2) * S) == t->color[TC_BUTTON_PRESSED],
          "a pressed colour button: %06x", pixel(win, (bx + cb->w - 4) * S, (by + cb->h / 2) * S));
    /* The release outside the button opens no dialog. */
    struct wmsg up = mouse_msg(win, WMOUSE_UP, bx + cb->w / 2, by + cb->h + 40, 0);
    window_message(win, &up);
    window_paint(win);
    CHECK(pixel(win, (bx + cb->w - 4) * S, (by + cb->h / 2) * S) == t->color[TC_BUTTON], "the button is released");

    int lx, ly, lh = t->font->height + 4;
    widget_abs(list, &lx, &ly);
    click(win, lx + 20, ly + 1 + lh + lh / 2);
    CHECK(activated == -1 && list->value == 1, "a single click selects the row");
    click(win, lx + 20, ly + 1 + lh + lh / 2);
    CHECK(activated == 1, "a double click activates the row: %d", activated);
}

static void test_icons(void)
{
    icon_set_dir("tests/data");
    const struct image *one = icon_lookup("shape", 16, 1, ICON_COLOR_DEFAULT);
    const struct image *two = icon_variant(one, 2, ICON_COLOR_DEFAULT);
    CHECK(one && one->w == 16 && one->scale == 1, "the icon at scale 1");
    CHECK(two && two != one && two->w == 32 && two->scale == 2, "the icon at scale 2");
    CHECK(icon_variant(two, 1, ICON_COLOR_DEFAULT) == one, "the cache returns the rendition at scale 1");
    const struct image *red = icon_variant(one, 2, 0x00ff0000);
    CHECK(red && red != two && red->w == 32, "a rendition in another colour");
    icon_set_dir("/usr/share/icons");
}

void run_scale_tests(void)
{
    struct app *a = app_create_detached();
    struct theme *t = app_theme(a);
    strlcpy(t->font_path, "../../third_party/dejavu/DejaVuSans.ttf", sizeof t->font_path);
    t->fallback_path[0] = '\0';
    theme_apply(t);
    CHECK(t->font != gfx_font_builtin(), "DejaVu Sans loads");
    fake_scale = S;
    struct surface none = { NULL, 0, 0, 0 };
    struct painter p;
    painter_init_scaled(&p, &none, t, S);
    test_textfield(a, &p);
    test_widths(a, &p);
    test_menu(a, &p);
    test_scrollbar(a, &p);
    test_buttons_and_lists(a, &p);
    test_icons();
    fake_scale = 1;
    app_destroy(a);
}
