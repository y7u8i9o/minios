#pragma once
/* Retained widget tree: classes, geometry, size hints, signals with
 * typed arguments, focus, and partial redraws. */
#include <gui/paint.h>
#include <gui/client.h>
#include <stddef.h>

struct app;
struct widget;
struct window_state;

/* ---- events delivered to widget classes ---- */

enum event_type {
    EV_MOUSE_DOWN, EV_MOUSE_UP, EV_MOUSE_MOVE, EV_MOUSE_WHEEL,
    EV_KEY_DOWN, EV_KEY_UP, EV_TEXT, EV_PREEDIT, EV_TEXT_DELETE,
    EV_FOCUS_IN, EV_FOCUS_OUT, EV_ENTER, EV_LEAVE,
};

struct event {
    enum event_type type;
    int x, y;                   /* local coordinates for mouse events */
    int button;                 /* bit mask of held buttons, or wheel delta */
    int mods;                   /* WMOD_* */
    int code, ch;               /* keys: scancode, translated character */
    const char *text;           /* EV_TEXT: committed UTF-8, event lifetime */
    int before, after;          /* EV_TEXT_DELETE: UTF-8 bytes around cursor */
};

/* ---- size hints and layout ---- */

struct size_hint {
    int min_w, min_h;
    int pref_w, pref_h;
    int max_w, max_h;           /* 0 means unbounded */
};

enum align { ALIGN_FILL = 0, ALIGN_START, ALIGN_CENTER, ALIGN_END };

/* ---- signals ---- */

struct sig_click { int button, x, y; };
struct sig_change { int value; const char *text; };
struct sig_key { int code, ch, mods; };
struct sig_select { int index; };
struct sig_resize { int w, h; };
struct sig_scroll { int value; };
struct sig_paint { struct painter *p; };

/* A handler returns non zero when it consumed the signal. */
typedef int (*signal_fn)(struct widget *w, void *args, void *arg);

struct handler {
    const char *name;
    signal_fn fn;
    void *arg;
    struct handler *next;
};

/* ---- classes ---- */

struct widget_class {
    const char *name;
    size_t size;                /* of the widget structure, at least sizeof(struct widget) */
    void (*measure)(struct widget *w, struct size_hint *h);
    void (*layout)(struct widget *w);           /* place the children */
    void (*paint)(struct widget *w, struct painter *p);
    int (*event)(struct widget *w, struct event *e);  /* returns 1 when consumed */
    void (*destroy)(struct widget *w);
};

struct widget {
    const struct widget_class *cls;
    struct app *app;
    struct widget *parent, *first, *last, *next, *prev;
    struct widget *window;      /* top level ancestor */
    int x, y, w, h;             /* inside the parent */
    unsigned visible : 1, enabled : 1, focusable : 1, focused : 1, hover : 1, pressed : 1;
    unsigned accepts_text : 1;
    unsigned dirty : 1, child_dirty : 1, needs_layout : 1;
    unsigned floating : 1;      /* positioned by its owner, skipped by layout (popups) */
    struct size_hint hint;      /* set by the application; measure fills the rest */
    struct size_hint measured;
    int stretch_x, stretch_y;
    enum align align_x, align_y;
    int margin;
    int padding;                /* inside containers; -1: theme padding for windows, 0 otherwise */
    /* grid placement */
    int row, col, row_span, col_span;
    char *id;
    char *text;
    char *tip;                  /* tooltip shown after hovering */
    const struct image *icon;   /* not owned; buttons and labels draw it */
    int value, min, max;        /* generic integer state */
    struct handler *handlers;
    int accel_key, accel_mods;
    void *user;
};

/* ---- construction and tree ---- */

struct widget *widget_new(const struct widget_class *cls, struct widget *parent);
void widget_destroy(struct widget *w);
void widget_add(struct widget *parent, struct widget *child);
void widget_remove(struct widget *child);
struct widget *widget_find(struct widget *root, const char *id);
void widget_set_id(struct widget *w, const char *id);

/* ---- properties (each invalidates or relayouts as needed) ---- */

void widget_set_text(struct widget *w, const char *text);
const char *widget_text(const struct widget *w);
void widget_set_value(struct widget *w, int value);
void widget_set_range(struct widget *w, int min, int max);
void widget_set_visible(struct widget *w, int visible);
void widget_set_enabled(struct widget *w, int enabled);
void widget_set_hint(struct widget *w, int pref_w, int pref_h);
void widget_set_min(struct widget *w, int min_w, int min_h);
void widget_set_max(struct widget *w, int max_w, int max_h);
void widget_set_stretch(struct widget *w, int x, int y);
void widget_set_align(struct widget *w, enum align x, enum align y);
void widget_set_grid(struct widget *w, int row, int col, int row_span, int col_span);
void widget_set_accel(struct widget *w, int key, int mods);
void widget_set_padding(struct widget *w, int padding);
void widget_set_tip(struct widget *w, const char *tip);
void widget_set_icon(struct widget *w, const struct image *icon);

/* ---- signals ---- */

void widget_connect(struct widget *w, const char *signal, signal_fn fn, void *arg);
int widget_emit(struct widget *w, const char *signal, void *args);

/* ---- redraw, layout and focus ---- */

void widget_invalidate(struct widget *w);
void widget_relayout(struct widget *w);
void widget_focus(struct widget *w);
struct widget *widget_focused(struct widget *window);
void widget_capture(struct widget *w);          /* mouse events until release */
void widget_abs(const struct widget *w, int *x, int *y);   /* position in the window */
struct widget *widget_at(struct widget *w, int x, int y);   /* deepest visible child */
const struct theme *widget_theme(const struct widget *w);
/* Deliver an event to a widget and let it bubble to its ancestors. */
int widget_dispatch(struct widget *w, struct event *e);

/* ---- windows ---- */

/* A top level window; created with app_window. Signals: "close",
 * "resize" (sig_resize), "focus" (sig_change value 1/0). */
extern const struct widget_class window_class;
struct window_state {
    struct gui_window *win;
    struct widget *focus, *capture, *hover;
    struct rect damage;         /* union of repainted areas */
    int has_damage;
    int closed;
    struct widget *popup;       /* floating child closed by an outside click */
    struct gui_window *popup_win; /* compositor popup surface when available */
    struct widget *tip;         /* the tooltip label, when shown */
    struct widget *tip_owner;
    struct timer *tip_timer;
};
struct window_state *window_state_of(struct widget *window);
/* Route a server message to the window (also used by the tests). */
void window_message(struct widget *window, struct wmsg *m);
/* Layout and paint what changed; returns the damaged rectangle. */
struct rect window_paint(struct widget *window);
void window_close(struct widget *window);
/* Show w as a floating popup of the window at window coordinates; it is
 * destroyed by window_popup_close, by a click outside it or by Escape. */
void window_popup_open(struct widget *window, struct widget *w, int x, int y, int width, int height);
void window_popup_close(struct widget *window);
int window_owns_id(struct widget *window, int id);

/* ---- containers and core widgets (the widgets directory) ---- */

extern const struct widget_class box_class;     /* value: 1 vertical, 0 horizontal */
extern const struct widget_class grid_class;
extern const struct widget_class label_class;
extern const struct widget_class button_class;  /* "clicked" (sig_click) */
extern const struct widget_class checkbox_class;/* "toggled" (sig_change) */
extern const struct widget_class radio_class;   /* "toggled"; exclusive among siblings */
extern const struct widget_class textfield_class;  /* "changed", "activate" (sig_change) */
extern const struct widget_class listview_class;   /* "selected", "activate" (sig_select) */
extern const struct widget_class scrollbar_class;  /* "scrolled" (sig_scroll) */
extern const struct widget_class scrollarea_class; /* one child, scrolled by two bars */
extern const struct widget_class canvas_class;     /* "paint" (sig_paint) */
extern const struct widget_class separator_class;

struct widget *box_new(struct widget *parent, int vertical);
struct widget *grid_new(struct widget *parent);
void grid_set_stretch(struct widget *grid, int row, int col, int stretch);   /* -1 leaves one unchanged */
struct widget *label_new(struct widget *parent, const char *text);
struct widget *button_new(struct widget *parent, const char *text);
struct widget *checkbox_new(struct widget *parent, const char *text);
struct widget *radio_new(struct widget *parent, const char *text);
struct widget *textfield_new(struct widget *parent, const char *text);
struct widget *listview_new(struct widget *parent);
void listview_clear(struct widget *w);
void listview_add(struct widget *w, const char *item);
int listview_count(const struct widget *w);
const char *listview_item(const struct widget *w, int index);
struct widget *scrollbar_new(struct widget *parent, int vertical);
void scrollbar_set(struct widget *w, int value, int max, int page);
struct widget *scrollarea_new(struct widget *parent);
struct widget *canvas_new(struct widget *parent);
struct widget *separator_new(struct widget *parent);

/* ---- controls (widgets/controls.c) ---- */

extern const struct widget_class combobox_class;   /* "changed" (sig_select); value: index */
extern const struct widget_class spinner_class;    /* "changed" (sig_change value) */
extern const struct widget_class slider_class;     /* "changed" (sig_change value) */
extern const struct widget_class progress_class;

struct widget *combobox_new(struct widget *parent);
void combobox_add(struct widget *w, const char *item);
void combobox_clear(struct widget *w);
void combobox_select(struct widget *w, int index);
const char *combobox_item(const struct widget *w, int index);
struct widget *spinner_new(struct widget *parent, int min, int max, int value);
struct widget *slider_new(struct widget *parent, int min, int max, int value);
struct widget *progress_new(struct widget *parent);

/* ---- containers (widgets/containers.c) ---- */

extern const struct widget_class tabs_class;       /* "changed" (sig_select); value: current page */
extern const struct widget_class splitpane_class;  /* value: 1 vertical split (top/bottom) */
extern const struct widget_class toolbar_class;
extern const struct widget_class statusbar_class;

struct widget *tabs_new(struct widget *parent);
struct widget *tabs_add(struct widget *tabs, const char *title);    /* returns the page (a vertical box) */
void tabs_select(struct widget *tabs, int index);
/* Hide the title row while the tabs hold a single page. */
void tabs_set_autohide(struct widget *tabs, int on);
struct widget *splitpane_new(struct widget *parent, int vertical);  /* two children added by the caller */
void splitpane_set_position(struct widget *w, int pos);
struct widget *toolbar_new(struct widget *parent);
struct widget *toolbar_add(struct widget *toolbar, const char *icon, const char *tip);  /* a button */
struct widget *statusbar_new(struct widget *parent);
struct widget *statusbar_add(struct widget *bar, int stretch);      /* a label */

/* ---- data views (widgets/models.c), see gui/model.h ---- */

/* "selected" and "activate" (sig_select: row id); "activate" also on a
 * double click. A right click selects the row and emits "context"
 * (sig_click, local coordinates) for a popup menu. */
extern const struct widget_class treeview_class;
extern const struct widget_class table_class;
struct model;
struct widget *treeview_new(struct widget *parent);
struct widget *table_new(struct widget *parent);
void view_set_model(struct widget *w, struct model *m);   /* both classes */
void view_refresh(struct widget *w);                      /* after the model changed */
int view_visible_rows(const struct widget *w);           /* flattened rows */
int view_row_at(const struct widget *w, int index);      /* row id of a flattened row */
void view_select(struct widget *w, int row);             /* select and scroll to a row id */
void view_scroll_to(struct widget *w, int row);          /* scroll to a row id, the selection unchanged */
int view_scroll_position(const struct widget *w);        /* the first visible flattened row */
void treeview_expand(struct widget *w, int row, int expanded);
int treeview_is_expanded(const struct widget *w, int row);
void table_set_column_width(struct widget *w, int col, int width);
int table_column_width(const struct widget *w, int col);

/* ---- menus (widgets/menu.c) ---- */

extern const struct widget_class menubar_class;
extern const struct widget_class menu_class;
extern const struct widget_class menuitem_class;   /* "clicked" (sig_click) */
struct widget *menubar_new(struct widget *parent);
struct widget *menu_new(struct widget *menubar, const char *title);
struct widget *menu_add(struct widget *menu, const char *text, const char *icon);
struct widget *menu_add_separator(struct widget *menu);
void menubar_open(struct widget *menubar, int index);   /* -1 closes */
/* A menu without a bar, shown by menu_popup at window coordinates (a
 * context menu); items are added with menu_add. */
struct widget *popupmenu_new(struct widget *window);
void menu_popup(struct widget *menu, int x, int y);

/* ---- text editor (widgets/editor.c) ---- */

extern const struct widget_class editor_class;     /* "changed", "cursor" (sig_change value: line) */
/* Character classes produced by a highlighter. */
enum hl_class { HL_NORMAL, HL_KEYWORD, HL_STRING, HL_COMMENT, HL_NUMBER, HL_PREPROC };
/* Fill classes[0..len) for one line; state carries across lines (block
 * comments) and starts at 0 for the first line. */
typedef void (*highlight_fn)(const char *line, int len, unsigned char *classes, int *state, void *arg);
/* A language for highlight_lang: keywords (NULL terminated), the prefix of
 * a line comment, the delimiters of a block comment, the quote characters
 * of strings, and the character that starts a preprocessor line; each may
 * be NULL or 0. highlight_lang receives a pointer to it as arg. */
struct highlight_language {
    const char *const *keywords;
    const char *line_comment;
    const char *block_start, *block_end;
    const char *quotes;
    int preproc;
};
void highlight_lang(const char *line, int len, unsigned char *classes, int *state, void *arg);
extern const struct highlight_language highlight_language_c, highlight_language_lua, highlight_language_sh;
struct widget *editor_new(struct widget *parent);
void editor_set_text(struct widget *w, const char *text);
char *editor_text(struct widget *w);                 /* malloc'ed, joined with newlines */
int editor_line_count(const struct widget *w);
const char *editor_line(const struct widget *w, int index);
void editor_set_wrap(struct widget *w, int on);
void editor_set_line_numbers(struct widget *w, int on);
void editor_set_readonly(struct widget *w, int on);
void editor_set_highlighter(struct widget *w, highlight_fn fn, void *arg);
/* An outline font of the editor's own at px logical pixels, NULL for the
 * theme's font. Returns 0 or -errno. */
int editor_set_font(struct widget *w, const char *path, int px);
void editor_goto(struct widget *w, int line, int col);
void editor_cursor(const struct widget *w, int *line, int *col);
int editor_undo(struct widget *w);
int editor_redo(struct widget *w);
int editor_can_undo(const struct widget *w);
int editor_can_redo(const struct widget *w);
/* The selection and the clipboard.  editor_selection returns the selected
 * text as a malloc'ed string or NULL.  A right click opens a context menu
 * with the same commands. */
int editor_has_selection(const struct widget *w);
char *editor_selection(struct widget *w);
void editor_cut(struct widget *w);
void editor_copy(struct widget *w);
void editor_paste(struct widget *w);
void editor_delete_selection(struct widget *w);
void editor_select_all(struct widget *w);
/* Replace every occurrence of needle, which may not contain a newline, as
 * one undo step.  Returns the number of replacements. */
int editor_replace_all(struct widget *w, const char *needle, const char *replacement);
int editor_find(struct widget *w, const char *needle, int forward);   /* 1 when found and selected */
int editor_modified(const struct widget *w);
void editor_set_modified(struct widget *w, int modified);
/* Built in highlighters. */
void highlight_c(const char *line, int len, unsigned char *classes, int *state, void *arg);
void highlight_sh(const char *line, int len, unsigned char *classes, int *state, void *arg);

/* Icons from /usr/share/icons: <name>.svg rendered 16 logical pixels
 * high at the output's scale, else <name>.png; cached per process. */
const struct image *icon_get(const char *name);
/* The SVG icon rendered px logical pixels high; NULL without an SVG. */
const struct image *icon_get_size(const char *name, int px);
/* A copy of an icon with reduced opacity for disabled buttons and menu
 * items, cached per icon. */
const struct image *icon_dimmed(const struct image *img);
