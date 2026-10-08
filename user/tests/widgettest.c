/* Framework test client for the gui_widgets boot test: a window whose
 * widgets log what they receive. With "controls" it builds the M22
 * controls window for gui_controls instead. With "scale" it builds the
 * text field of gui_widgets_scale2. With "look" it shows the controls
 * of gui_widgets_look, whose pixels the kernel test samples. With "text"
 * it builds the menu, the text field and the spinner of gui_widgets_text.
 * Hints fix
 * the sizes. The kernel
 * test can therefore click at known positions. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gui/app.h>

static struct app *app;

#define LOG(...) do { printf("widgettest: " __VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static int field_changed(struct widget *w, void *args, void *arg) { LOG("field '%s'", widget_text(w)); return 0; }
static int area_changed(struct widget *w, void *args, void *arg)
{
    char *t = editor_text(w);
    LOG("area %d bytes", (int)strlen(t));
    free(t);
    return 0;
}
static int check_clicked(struct widget *w, void *args, void *arg) { LOG("check %d", w->value); return 0; }
static int scroll_changed(struct widget *w, void *args, void *arg) { LOG("scroll %d", w->value); return 0; }
static int list_selected(struct widget *w, void *args, void *arg) { LOG("select %d", w->value); return 0; }
static int menu_new_(struct widget *w, void *args, void *arg) { LOG("menu New"); return 1; }
static int menu_quit(struct widget *w, void *args, void *arg) { LOG("menu Quit"); app_quit(app, 0); return 1; }
static int combo_changed(struct widget *w, void *args, void *arg) { LOG("combo %d '%s'", w->value, widget_text(w)); return 0; }
static int spin_changed(struct widget *w, void *args, void *arg) { LOG("spinner %d", w->value); return 0; }
static int slider_changed(struct widget *w, void *args, void *arg) { LOG("slider %d", w->value); return 0; }
static int tab_changed(struct widget *w, void *args, void *arg) { LOG("tab %d", w->value); return 0; }
static int quit_clicked(struct widget *w, void *args, void *arg) { LOG("quit"); app_quit(app, 0); return 1; }

/* The scale mode quits after the first change of its field. */
static int scale_field_changed(struct widget *w, void *args, void *arg)
{
    LOG("field '%s'", widget_text(w));
    app_quit(app, 0);
    return 0;
}

static struct widget *positions[8];
static int npositions;

static void log_positions(void *arg)
{
    for (int i = 0; i < npositions; i++) {
        int x, y;
        widget_abs(positions[i], &x, &y);
        LOG("%s at %d,%d %dx%d", positions[i]->cls->name, x, y, positions[i]->w, positions[i]->h);
    }
}

static void build_widgets(struct widget *win)
{
    struct widget *bar = menubar_new(win);
    widget_set_hint(bar, 0, 26);
    struct widget *file = menu_new(bar, "File");
    widget_connect(menu_add(file, "New", NULL), "clicked", menu_new_, NULL);
    widget_connect(menu_add(file, "Quit", NULL), "clicked", menu_quit, NULL);
    struct widget *field = textfield_new(win, "");
    widget_set_hint(field, 0, 26);
    widget_connect(field, "changed", field_changed, NULL);
    struct widget *check = checkbox_new(win, "Option");
    widget_set_hint(check, 0, 26);
    widget_connect(check, "toggled", check_clicked, NULL);
    struct widget *row = box_new(win, 0);
    widget_set_stretch(row, 1, 1);
    row->margin = 0;
    struct widget *area = editor_new(row);
    widget_connect(area, "changed", area_changed, NULL);
    struct widget *sb = scrollbar_new(row, 1);
    scrollbar_set(sb, 0, 100, 10);
    widget_connect(sb, "scrolled", scroll_changed, NULL);
    positions[npositions++] = area;
    positions[npositions++] = sb;
    struct widget *list = listview_new(row);
    widget_set_hint(list, 120, 0);
    widget_set_stretch(list, 0, 1);
    for (int i = 0; i < 30; i++) {
        char item[16];
        snprintf(item, sizeof item, "item %d", i);
        listview_add(list, item);
    }
    widget_connect(list, "selected", list_selected, NULL);
    positions[npositions++] = list;
    positions[npositions++] = bar;
}

/* The field "Widgets" lies in a row after a gap. The gap places the
 * boundary between "Wid" and "gets" at x 200 of the window, measured at
 * the scale of the window. A click there must put the cursor after
 * "Wid". */
#define SCALE_BOUNDARY_X 200

static void build_scale(struct widget *win)
{
    const struct theme *t = app_theme(app);
    struct widget *row = box_new(win, 0);
    widget_set_padding(row, 0);
    struct widget *gap = label_new(row, "");
    struct widget *field = textfield_new(row, "Widgets");
    widget_set_hint(field, 0, 26);
    widget_connect(field, "changed", scale_field_changed, NULL);
    /* The window padding, the spacing of the row, and the padding of 4
     * pixels inside the field precede the text. */
    int text_x = theme_px(t, TM_PADDING) + theme_px(t, TM_SPACING) + 4;
    widget_set_hint(gap, SCALE_BOUNDARY_X - text_x - widget_text_width(field, NULL, "Wid", 3), 26);
    LOG("scale %d", widget_scale(win));
    positions[npositions++] = field;
}

/* A text field at the top of the window, then controls for the look. */
static void build_look(struct widget *win)
{
    struct widget *field = textfield_new(win, "Field");
    widget_set_hint(field, 0, 26);
    widget_set_value(checkbox_new(win, "Check box"), 1);
    widget_set_value(radio_new(win, "Radio button"), 1);
    struct widget *combo = combobox_new(win);
    combobox_add(combo, "Combo box");
    spinner_new(win, 0, 10, 5);
    LOG("look ready");
}

static int menu_wrap(struct widget *w, void *args, void *arg)
{
    LOG("menu Wrap %d", menuitem_checked(w));
    return 1;
}
static int menu_small(struct widget *w, void *args, void *arg) { LOG("menu Small"); return 1; }
static int menu_large(struct widget *w, void *args, void *arg) { LOG("menu Large"); app_quit(app, 0); return 1; }

static void build_text(struct widget *win)
{
    struct widget *bar = menubar_new(win);
    widget_set_hint(bar, 0, 26);
    struct widget *view = menu_new(bar, "View");
    struct widget *wrap = menu_add(view, "Wrap", NULL);
    menuitem_set_check(wrap, 0);
    widget_connect(wrap, "clicked", menu_wrap, NULL);
    struct widget *size = menu_add_submenu(view, "Size");
    widget_connect(menu_add(size, "Small", NULL), "clicked", menu_small, NULL);
    widget_connect(menu_add(size, "Large", NULL), "clicked", menu_large, NULL);
    struct widget *field = textfield_new(win, "");
    widget_set_hint(field, 0, 26);
    widget_connect(field, "changed", field_changed, NULL);
    struct widget *spin = spinner_new(win, 0, 10, 5);
    widget_set_hint(spin, 0, 26);
    widget_connect(spin, "changed", spin_changed, NULL);
}

static void build_controls(struct widget *win)
{
    struct widget *combo = combobox_new(win);
    widget_set_hint(combo, 0, 26);
    combobox_add(combo, "red");
    combobox_add(combo, "green");
    combobox_add(combo, "blue");
    widget_connect(combo, "changed", combo_changed, NULL);
    struct widget *spin = spinner_new(win, 0, 10, 5);
    widget_set_hint(spin, 0, 26);
    widget_connect(spin, "changed", spin_changed, NULL);
    struct widget *slider = slider_new(win, 0, 100, 0);
    widget_set_hint(slider, 0, 26);
    widget_connect(slider, "changed", slider_changed, NULL);
    struct widget *tabs = tabs_new(win);
    struct widget *p1 = tabs_add(tabs, "First"), *p2 = tabs_add(tabs, "Second");
    label_new(p1, "page one");
    struct widget *quit = button_new(p2, "Quit");
    widget_set_hint(quit, 0, 30);
    widget_connect(quit, "clicked", quit_clicked, NULL);
    widget_connect(tabs, "changed", tab_changed, NULL);
    struct widget *prog = progress_new(win);
    widget_set_value(prog, 30);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "icons") == 0) {
        const char *names[] = { "new", "open", "save", "quit", "search", "up", NULL };
        for (int i = 0; names[i]; i++) {
            const struct image *img = icon_get(names[i]);
            LOG("icon %s %dx%d", names[i], img ? img->w : -1, img ? img->h : -1);
        }
        LOG("done");
        return 0;
    }
    app = app_create();
    if (!app)
        return 1;
    const char *mode = argc > 1 ? argv[1] : "widgets";
    struct widget *win = app_window(app, 400, 300, mode);
    if (!win)
        return 1;
    LOG("theme font height %d", app_theme(app)->font->height);
    if (strcmp(mode, "controls") == 0)
        build_controls(win);
    else if (strcmp(mode, "scale") == 0)
        build_scale(win);
    else if (strcmp(mode, "look") == 0)
        build_look(win);
    else if (strcmp(mode, "text") == 0)
        build_text(win);
    else
        build_widgets(win);
    app_timer_add(app, 300, 0, log_positions, NULL);
    app_run(app);
    app_destroy(app);
    LOG("done");
    return 0;
}
