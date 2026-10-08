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
    EV_DRAG_MOVE, EV_DRAG_LEAVE, EV_DROP, EV_DRAG_END,
};

/* Drag and drop (docs/design/dnd.md). EV_DRAG_MOVE: a drag is over the
 * widget; a widget that takes drops sets accept_mime (one of the offered
 * types, see widget_drag_offers) and the actions and returns 1, and a
 * widget that refuses for its ancestors as well returns 1 without them.
 * EV_DRAG_LEAVE: the drag no longer targets the widget, after a drop as
 * well. EV_DROP: the data, delivered to the widget that accepted last.
 * EV_DRAG_END goes to the widget that started a drag, with the action
 * performed or 0 when the drag was cancelled. */
struct drag_event {
    int actions;                /* allowed by the source (GUI_DND_*) */
    int action;                 /* chosen by the compositor; performed for EV_DROP and EV_DRAG_END */
    const char *mime;           /* EV_DROP */
    const char *data;           /* EV_DROP, terminated by a NUL byte */
    size_t len;
    const char *accept_mime;    /* the answer to EV_DRAG_MOVE */
    int accept_actions, preferred;
};

struct event {
    enum event_type type;
    int x, y;                   /* local coordinates for mouse events */
    int button;                 /* bit mask of pressed buttons, or wheel delta */
    int mods;                   /* WMOD_* */
    int code, ch;               /* keys: scancode, translated character */
    const char *text;           /* EV_TEXT: committed UTF-8, event lifetime */
    int before, after;          /* EV_TEXT_DELETE: UTF-8 bytes around cursor */
    struct drag_event *drag;    /* the drag events */
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
struct sig_text { const char *text; };    /* "text" and "preedit" of a canvas with accepts_text */
struct sig_select { int index; };
struct sig_resize { int w, h; };
struct sig_scroll { int value; };
struct sig_paint { struct painter *p; };
/* Drags of the data views: the row id (-1 for the area below the rows),
 * local coordinates and the drag event. A "drag_motion" handler may set
 * row to -1 to mark the whole view as the target. */
struct sig_drag { int row, x, y; struct drag_event *drag; };

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
/* Drags. A widget starts one when the pointer has moved more than
 * DRAG_THRESHOLD logical pixels from the press (widget_drag_moved).
 * widget_drag_start takes the items of gui_drag_start and draws the drag
 * image from icon and label (both may be NULL); w receives EV_DRAG_END. */
#define DRAG_THRESHOLD 4
int widget_drag_moved(int press_x, int press_y, int x, int y);
/* Repeated clicks. gui_click_count records a press at (x, y) and returns
 * the count of the press: 1 for a single click, 2 for a double click and
 * 3 for a triple click. A press repeats the previous press when it follows
 * within GUI_DOUBLE_CLICK_MS and lies within DRAG_THRESHOLD of the
 * previous press. The count after 3 starts again at 1. */
struct gui_clicks {
    long ms;
    int x, y, count;
};
int gui_click_count(struct gui_clicks *c, int x, int y);
int widget_drag_start(struct widget *w, const struct gui_drag_item *items, int nitems, int actions,
                      const struct image *icon, const char *label);
int widget_drag_offers(const char *mime);
void widget_abs(const struct widget *w, int *x, int *y);   /* position in the window */
/* A focused text widget reports its caret, in its own coordinates, for
 * the candidates of an input method. */
void widget_text_cursor(struct widget *w, int x, int y, int width, int height);
struct widget *widget_at(struct widget *w, int x, int y);   /* deepest visible child */
const struct theme *widget_theme(const struct widget *w);
/* The device pixels per logical pixel of the surface that shows w. The
 * value is the scale of the window of w, or of the popup surface for a
 * widget in a popup. The value is 1 for a widget without a window. */
int widget_scale(const struct widget *w);
/* Text measured at the scale of the window of w, with the same results
 * as painter_text_width and painter_text_index in a paint of w. A NULL
 * font is the font of the theme. widget_text_width returns the width of
 * the first n bytes (n < 0: all) in logical pixels rounded up.
 * widget_text_index returns the character boundary nearest to the
 * logical offset px. */
int widget_text_width(const struct widget *w, const struct font *font, const char *text, int n);
int widget_text_index(const struct widget *w, const struct font *font, const char *text, int n, int px);
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
    struct widget *drag_source;     /* started the drag that runs */
    struct widget *drop_target;     /* accepted the drag over the window last */
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
/* "selected", "activate" (sig_select); "activate" also on a double click */
extern const struct widget_class listview_class;
extern const struct widget_class scrollbar_class;  /* "scrolled" (sig_scroll) */
extern const struct widget_class scrollarea_class; /* one child, scrolled by two bars */
/* "paint" (sig_paint); "press", "motion", "release", "wheel" (sig_click);
 * "key", "keyup"; "text", "preedit"; the drag signals of the data views
 * (sig_drag, row -1). A canvas is not focusable by default. A program
 * whose canvas takes keys sets the flag focusable. */
extern const struct widget_class canvas_class;
extern const struct widget_class separator_class;

struct widget *box_new(struct widget *parent, int vertical);
struct widget *grid_new(struct widget *parent);
void grid_set_stretch(struct widget *grid, int row, int col, int stretch);   /* -1 leaves one unchanged */
struct widget *label_new(struct widget *parent, const char *text);
struct widget *button_new(struct widget *parent, const char *text);
struct widget *checkbox_new(struct widget *parent, const char *text);
struct widget *radio_new(struct widget *parent, const char *text);
struct widget *textfield_new(struct widget *parent, const char *text);
/* A masked field shows one '*' per byte of its text and refuses to copy
 * it, for passwords. */
void textfield_set_masked(struct widget *w, int masked);
/* Put the cursor at byte offset cursor, or at the end for -1, and select
 * from anchor to it, or nothing for an anchor of -1. */
void textfield_select(struct widget *w, int anchor, int cursor);
struct widget *listview_new(struct widget *parent);
void listview_clear(struct widget *w);
void listview_add(struct widget *w, const char *item);
int listview_count(const struct widget *w);
const char *listview_item(const struct widget *w, int index);
struct widget *scrollbar_new(struct widget *parent, int vertical);
void scrollbar_set(struct widget *w, int value, int max, int page);

/* Scroll ranges. A range has the length max, and page units of the range
 * are visible. The value is the first visible unit, from 0 to max - page.
 * scroll_clamp returns v limited to that interval. scroll_set stores the
 * clamped v in *value and returns 1 when *value changed. */
int scroll_clamp(int v, int max, int page);
int scroll_set(int *value, int v, int max, int page);
/* The thumb of a scroll track of len logical pixels. scrollbar_thumb
 * stores the offset of the thumb along the track in *off and its length in
 * *len_out. The function returns the distance that the thumb can travel,
 * or 0 when the whole range is visible. Painting and hit testing use this
 * geometry. */
int scrollbar_thumb(int len, int value, int max, int page, int *off, int *len_out);
/* The track and the thumb of a scroll bar at (x, y). */
void scrollbar_paint_track(struct painter *p, int x, int y, int w, int h, int value, int max, int page, int vertical);
/* A vertical scroll track inside a widget: the list view, the tree view,
 * the table and the editor. scroll_track_event handles a mouse event of
 * the widget w for the track at the local rectangle r. A press before or
 * after the thumb moves the value by one page. A press on the thumb
 * captures the mouse, and the motion drags the thumb. The function stores
 * the new value in *value and returns 1 when the track consumed the event. */
struct scroll_track {
    int grab;                   /* the press offset inside the thumb, -1 without a drag */
};
int scroll_track_event(struct scroll_track *t, struct widget *w, const struct event *e, struct rect r, int *value,
                       int max, int page);
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

/* ---- graph (widgets/graph.c) ---- */

/* A graph of up to GRAPH_SERIES time series. It retains the last samples
 * values of each series, the newest at the right edge. A heading row
 * shows the title and on the right the value text, or the legend of the
 * series when no value text is set. A small graph omits the heading row.
 * Below it, a framed plot with a grid of four rows shows each series by
 * its style. The vertical scale
 * is the fixed maximum, or with automatic scaling graph_nice_max of the
 * largest retained value. With a format function the plot shows the
 * scale in its top left corner. */
#define GRAPH_SERIES 4
enum graph_style {
    GRAPH_LINE,                 /* a line and a coloured square in the legend */
    GRAPH_AREA,                 /* the line and the area under it */
    GRAPH_TEXT,                 /* no line, the label without a square in the legend */
};
extern const struct widget_class graph_class;
struct widget *graph_new(struct widget *parent, int samples);
/* Adds a series and returns its index, or -1 when GRAPH_SERIES exist.
 * Colour 0 is the accent colour of the theme for the first series and
 * the text colour for the others. A series with an empty label has no
 * legend item. */
int graph_add_series(struct widget *g, const char *label, uint32_t color, enum graph_style style);
void graph_set_label(struct widget *g, int series, const char *label);
void graph_set_style(struct widget *g, int series, enum graph_style style);
/* Appends one sample to every series. values has one entry per series. */
void graph_push(struct widget *g, const long *values);
void graph_clear(struct widget *g);
/* automatic 0: the scale is max. automatic 1: the scale is
 * graph_nice_max(largest value, max). */
void graph_set_scale(struct widget *g, long max, int automatic);
void graph_set_title(struct widget *g, const char *title, const char *value);
void graph_set_format(struct widget *g, void (*format)(long value, char *buf, size_t size));
/* The current scale. */
long graph_scale(const struct widget *g);
/* The smallest value of 1, 2 or 5 times a power of ten that is at least
 * v, and at least minimum. */
long graph_nice_max(long v, long minimum);

/* ---- colours (widgets/color.c) ---- */

/* A modal dialog that chooses a colour: a field of saturation and value
 * for the hue of a strip beside it, the old and the new colour, and the
 * hexadecimal value, which accepts the forms of gfx_color_parse. Returns
 * 1 and sets *color when the user accepts, 0 otherwise. parent NULL is
 * the first window of the application. */
int color_dialog(struct app *a, struct widget *parent, const char *title, uint32_t *color);
/* A button with a swatch and the hexadecimal value of a colour. It opens
 * color_dialog with title and emits "changed" (sig_change, value the new
 * 0x00RRGGBB colour) after an accepted change. */
extern const struct widget_class color_button_class;
struct widget *colorbutton_new(struct widget *parent, uint32_t color, const char *title);
uint32_t colorbutton_color(const struct widget *w);
/* Sets the colour without a signal. */
void colorbutton_set(struct widget *w, uint32_t color);

/* ---- containers (widgets/containers.c) ---- */

extern const struct widget_class tabs_class;       /* "changed" (sig_select); value: current page */
extern const struct widget_class splitpane_class;  /* value: 1 vertical split (top/bottom) */
extern const struct widget_class toolbar_class;
extern const struct widget_class statusbar_class;

struct widget *tabs_new(struct widget *parent);
struct widget *tabs_add(struct widget *tabs, const char *title);    /* returns the page (a vertical box) */
void tabs_select(struct widget *tabs, int index);
/* Hide the title row while the tabs contain a single page. */
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
 * (sig_click, local coordinates) for a popup menu. Drags (sig_drag): a
 * row pressed and moved past DRAG_THRESHOLD emits "drag_begin", whose
 * handler calls widget_drag_start; "drag_end" follows. A drag over the
 * view emits "drag_motion", whose handler accepts as for EV_DRAG_MOVE and
 * returns 1, then "drop" on the accepted row or view, and "drag_leave".
 * The accepted row, or the whole view, is outlined in the accent colour. */
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
/* The rectangle of a row id in the coordinates of the view. Returns 0 when
 * the row is not visible. */
int view_row_rect(struct widget *w, int row, struct rect *r);
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
/* The icon cache. The key of an entry consists of name, size, scale and
 * colour. icon_lookup returns the SVG icon name rendered px logical pixels high at
 * scale device pixels per logical pixel in color, or NULL without an SVG
 * file. ICON_COLOR_DEFAULT is the colour of the icon theme: the text
 * colour, amber for folders and red for the quit mark. The cache grows as
 * needed. A missing icon is cached as NULL. */
#define ICON_COLOR_DEFAULT 0xffffffffu
/* The directory of the icon files, /usr/share/icons by default. The host
 * tests of libgui use their data directory. The string is not copied. */
void icon_set_dir(const char *dir);
const struct image *icon_lookup(const char *name, int px, int scale, uint32_t color);
/* The rendition of a cached icon at another scale and colour. The result
 * is img itself for an image that the cache did not create, such as a PNG
 * icon, and when the SVG file cannot be rendered again. */
const struct image *icon_variant(const struct image *img, int scale, uint32_t color);
/* Draws a cached icon at (x, y) at the scale of the painter in the colour
 * of the icon. With dimmed nonzero, the function draws the copy that
 * icon_dimmed returns.
 * Other images are drawn as painter_image draws them. */
void painter_icon(struct painter *p, int x, int y, const struct image *img, int dimmed);
/* A copy of an icon with reduced opacity for disabled buttons and menu
 * items, cached per icon. */
const struct image *icon_dimmed(const struct image *img);
