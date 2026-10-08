/* K6 of docs/plan/widgets.md: undo, word movement and word selection in
 * the text field, the placeholder, the caret blink, typed numbers in the
 * spinner, the editable combo box, label wrap and ellipsis, the pulsing
 * progress bar, check, radio and submenu items, and the image view. */
#include <gui/app.h>
#include <gui/image.h>
#include <stdio.h>
#include <string.h>
#include "check.h"
#include "events.h"
#include "fake.h"

struct app *app_create_detached(void);

static void key(struct widget *win, int code, int ch, int mods)
{
    struct wmsg m = key_msg(win, code, ch, mods);
    window_message(win, &m);
}

static uint32_t pixel(struct widget *win, int x, int y)
{
    struct surface *s = &window_state_of(win)->win->surf;
    return s->pixels[(size_t)y * s->stride + x] & 0xffffff;
}

static int count(struct widget *win, struct widget *w, uint32_t color)
{
    int ax, ay, n = 0;
    widget_abs(w, &ax, &ay);
    for (int y = ay; y < ay + w->h; y++)
        for (int x = ax; x < ax + w->w; x++)
            n += pixel(win, x, y) == color;
    return n;
}

static void test_textfield(struct app *a)
{
    const struct theme *t = app_theme(a);
    struct widget *win = app_window(a, 300, 100, "field");
    struct widget *f = textfield_new(win, "");
    textfield_set_placeholder(f, "Search");
    window_paint(win);
    CHECK(count(win, f, t->color[TC_TEXT_DISABLED]) > 0, "the placeholder of an empty field");
    widget_focus(f);
    type_text(win, "one two three");
    window_paint(win);
    CHECK(count(win, f, t->color[TC_TEXT_DISABLED]) == 0, "no placeholder with text");
    key(win, KEY_Z, 26, WMOD_CTRL);
    CHECK(strcmp(widget_text(f), "") == 0, "Ctrl+Z undoes the typed text: '%s'", widget_text(f));
    key(win, KEY_Y, 25, WMOD_CTRL);
    CHECK(strcmp(widget_text(f), "one two three") == 0, "Ctrl+Y redoes it: '%s'", widget_text(f));
    key(win, KEY_LEFT, 0, WMOD_CTRL);
    type_text(win, "|");
    CHECK(strcmp(widget_text(f), "one two |three") == 0, "Ctrl+Left moves to the word start: '%s'", widget_text(f));
    key(win, KEY_HOME, 0, 0);
    key(win, KEY_RIGHT, 0, WMOD_CTRL | WMOD_SHIFT);
    type_text(win, "1 ");
    CHECK(strcmp(widget_text(f), "1 two |three") == 0, "Ctrl+Shift+Right selects a word: '%s'", widget_text(f));

    /* A double click selects a word, a triple click the text. */
    window_paint(win);
    int fx, fy;
    widget_abs(f, &fx, &fy);
    int x = fx + 4 + widget_text_width(f, NULL, "1 t", 3);
    click(win, x, fy + f->h / 2);
    click(win, x, fy + f->h / 2);
    type_text(win, "2");
    CHECK(strcmp(widget_text(f), "1 2 |three") == 0, "a double click selects the word: '%s'", widget_text(f));
    /* The triple click lies away from the double click. The count of
     * the triple click therefore starts at 1. */
    x = fx + 4 + widget_text_width(f, NULL, "1 2 |th", 7);
    click(win, x, fy + f->h / 2);
    click(win, x, fy + f->h / 2);
    click(win, x, fy + f->h / 2);
    type_text(win, "all");
    CHECK(strcmp(widget_text(f), "all") == 0, "a triple click selects the text: '%s'", widget_text(f));

    /* A masked field records no history. */
    struct widget *m = textfield_new(win, "");
    textfield_set_masked(m, 1);
    widget_focus(m);
    type_text(win, "secret");
    key(win, KEY_Z, 26, WMOD_CTRL);
    CHECK(strcmp(widget_text(m), "secret") == 0, "a masked field has no undo");
    window_close(win);
}

/* The caret blinks while input is recent and remains visible after the
 * blink ends. */
static void test_blink(struct app *a)
{
    struct widget *win = app_window(a, 300, 100, "blink");
    struct widget *f = textfield_new(win, "text");
    widget_focus(f);
    window_paint(win);
    struct window_state *ws = window_state_of(win);
    CHECK(widget_caret_visible(f) && ws->blink_timer, "the caret starts visible and blinking");
    step_for(a, GUI_CARET_BLINK_MS + 100);
    CHECK(!widget_caret_visible(f), "the caret is hidden after one period");
    ws->last_input_ms -= 11000;
    step_for(a, GUI_CARET_BLINK_MS + 100);
    CHECK(widget_caret_visible(f) && !ws->blink_timer, "the blink ends visible 10 s after the input");
    window_close(win);
}

static int combo_index = 99;
static int on_combo(struct widget *w, void *args, void *arg)
{
    combo_index = ((struct sig_select *)args)->index;
    return 1;
}

static void test_controls(struct app *a)
{
    struct widget *win = app_window(a, 300, 200, "controls");
    struct widget *spin = spinner_new(win, 0, 50, 5);
    struct widget *combo = combobox_new(win);
    combobox_add(combo, "red");
    combobox_set_editable(combo, 1);
    widget_connect(combo, "changed", on_combo, NULL);
    window_paint(win);
    widget_focus(spin);
    type_text(win, "42\n");
    CHECK(spin->value == 42, "typed digits set the spinner: %d", spin->value);
    type_text(win, "99\n");
    CHECK(spin->value == 50, "a typed number is clamped: %d", spin->value);
    struct widget *field = combo->first;
    CHECK(field && field->cls == &textfield_class, "an editable combo box has a field");
    widget_focus(field);
    type_text(win, "x");
    CHECK(strcmp(widget_text(combo), "redx") == 0 && combo_index == -1, "typing edits the combo box: '%s' %d",
          widget_text(combo), combo_index);
    combobox_select(combo, 0);
    CHECK(strcmp(widget_text(field), "red") == 0, "a selected item sets the field");
    window_close(win);
}

static void test_label_progress(struct app *a)
{
    const struct theme *t = app_theme(a);
    struct widget *win = app_window(a, 160, 300, "labels");
    struct widget *wrap = label_new(win, "a long text that does not fit into one line of the window");
    label_set_wrap(wrap, 1);
    struct widget *ell = label_new(win, "another long text that does not fit into the window");
    label_set_ellipsis(ell, 1);
    struct widget *bar = progress_new(win);
    progress_set_pulse(bar, 1);
    window_paint(win);
    window_paint(win);
    CHECK(wrap->h >= 3 * t->font->height, "the wrapped label has several lines: %d pixels", wrap->h);
    CHECK(ell->h < 2 * t->font->height + 4 && ell->measured.min_w < ell->w, "the label with an ellipsis has one line");
    int accent = count(win, bar, t->color[TC_ACCENT]);
    CHECK(accent > 0 && accent < bar->w * bar->h / 2, "the pulse shows a block: %d pixels", accent);
    progress_set_pulse(bar, 0);
    window_close(win);
}

static int picked;
static int on_pick(struct widget *w, void *args, void *arg)
{
    picked = (int)(long)arg;
    return 1;
}

static void test_menus(struct app *a)
{
    struct widget *win = app_window(a, 400, 300, "menus");
    struct widget *m = popupmenu_new(win);
    struct widget *check = menu_add(m, "Check", NULL);
    menuitem_set_check(check, 0);
    struct widget *r1 = menu_add(m, "One", NULL), *r2 = menu_add(m, "Two", NULL);
    menuitem_set_radio(r1, 1);
    menuitem_set_radio(r2, 0);
    struct widget *sub = menu_add_submenu(m, "More");
    widget_connect(menu_add(sub, "Inner", NULL), "clicked", on_pick, (void *)7);
    menu_popup(m, 10, 10);
    key(win, KEY_DOWN, 0, 0);
    key(win, KEY_ENTER, '\n', 0);
    CHECK(menuitem_checked(check), "choosing a check item checks it");
    menu_popup(m, 10, 10);
    for (int i = 0; i < 3; i++)
        key(win, KEY_DOWN, 0, 0);
    key(win, KEY_ENTER, '\n', 0);
    CHECK(menuitem_checked(r2) && !menuitem_checked(r1), "a radio item clears the other");
    menu_popup(m, 10, 10);
    for (int i = 0; i < 4; i++)
        key(win, KEY_DOWN, 0, 0);
    key(win, KEY_RIGHT, 0, 0);
    key(win, KEY_ENTER, '\n', 0);
    CHECK(picked == 7, "Right opens the submenu and Enter chooses its item: %d", picked);
    window_close(win);
}

static void test_imageview(struct app *a)
{
    struct image *img = image_create(100, 50);
    for (int i = 0; i < 100 * 50; i++)
        img->pixels[i] = 0xffff0000u;
    struct widget *win = app_window(a, 100, 100, "image");
    widget_set_padding(win, 0);
    struct widget *v = imageview_new(win);
    widget_set_stretch(v, 1, 1);
    imageview_set(v, img);
    widget_set_max(v, 50, 50);
    window_paint(win);
    int vx, vy;
    widget_abs(v, &vx, &vy);
    CHECK(v->w == 50 && pixel(win, vx + 25, vy + 25) == 0xff0000 && pixel(win, vx + 25, vy + 5) != 0xff0000,
          "the image is reduced to 50x25 and centred");
    window_close(win);
    image_free(img);
}

void run_function_tests(void)
{
    struct app *a = app_create_detached();
    test_textfield(a);
    test_blink(a);
    test_controls(a);
    test_label_progress(a);
    test_menus(a);
    test_imageview(a);
    app_destroy(a);
}
