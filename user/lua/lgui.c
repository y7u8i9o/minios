/* The gui module: the libgui application object, windows, widgets,
 * signals, timers and descriptor watches. A widget is a full userdata
 * holding the C pointer; the registry table WIDGETS maps the pointer to
 * that userdata, so the same object is returned every time and the
 * "destroy" signal of libgui clears the pointer when the C widget dies.
 * Handlers live in the user value table of the widget. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#ifdef MINIOS_HOST
#include <poll.h>
#else
#include <minios/abi.h>          /* POLLIN, POLLOUT */
#endif
#include <gui/app.h>
#include "lauxlib.h"
#include "minios.h"
#include "lgui.h"

#ifdef MINIOS_HOST
struct app *app_create_detached(void);
#endif

static lua_State *gui_L;            /* the main thread, for callbacks from C */
static char widgets_key;            /* registry key of the widget table */

struct wref { struct widget *w; };
struct aref { struct app *a; };
struct tref { struct app *a; struct timer *t; int repeat; int self; };
struct href { struct app *a; struct watch *h; int self; };

/* ---- error reporting from callbacks ---- */

static int traceback(lua_State *L)
{
    const char *msg = lua_tostring(L, 1);
    luaL_traceback(L, L, msg ? msg : "(error object is not a string)", 1);
    return 1;
}

/* Calls fn with nargs arguments already on the stack; prints an error
 * with its traceback to stderr and returns 0, or returns nresults. */
int gui_call(lua_State *L, int nargs, int nresults)
{
    int base = lua_gettop(L) - nargs;
    lua_pushcfunction(L, traceback);
    lua_insert(L, base);
    int status = lua_pcall(L, nargs, nresults, base);
    if (status != LUA_OK) {
        fprintf(stderr, "lua: %s\n", lua_tostring(L, -1));
        lua_settop(L, base - 1);
        return 0;
    }
    lua_remove(L, base);
    return nresults;
}

/* ---- widget objects ---- */

static void widgets_table(lua_State *L)
{
    lua_rawgetp(L, LUA_REGISTRYINDEX, &widgets_key);
}

static int on_destroy(struct widget *w, void *args, void *arg)
{
    lua_State *L = gui_L;
    int top = lua_gettop(L);
    widgets_table(L);
    lua_rawgetp(L, -1, w);
    struct wref *r = lua_touserdata(L, -1);
    if (r)
        r->w = NULL;
    lua_pushnil(L);
    lua_rawsetp(L, -3, w);
    lua_settop(L, top);
    return 0;
}

/* Pushes the userdata of w, creating it on first use. */
void gui_push_widget(lua_State *L, struct widget *w)
{
    if (!w) {
        lua_pushnil(L);
        return;
    }
    widgets_table(L);
    lua_rawgetp(L, -1, w);
    if (!lua_isnil(L, -1)) {
        lua_remove(L, -2);
        return;
    }
    lua_pop(L, 1);
    struct wref *r = lua_newuserdatauv(L, sizeof *r, 1);
    r->w = w;
    luaL_setmetatable(L, GUI_WIDGET_META);
    lua_newtable(L);
    lua_setiuservalue(L, -2, 1);
    lua_pushvalue(L, -1);
    lua_rawsetp(L, -3, w);
    lua_remove(L, -2);
    widget_connect(w, "destroy", on_destroy, NULL);
}

struct widget *gui_check_widget(lua_State *L, int index)
{
    struct wref *r = luaL_checkudata(L, index, GUI_WIDGET_META);
    if (!r->w)
        luaL_error(L, "widget was destroyed");
    return r->w;
}

/* Handlers: name -> function in the user value table of the widget. */
static void push_handler_table(lua_State *L, int index)
{
    lua_getiuservalue(L, index, 1);
}

static int push_signal_args(lua_State *L, struct widget *w, const char *name, void *args)
{
    const char *cls = w->cls->name;
    if (strcmp(name, "clicked") == 0) {
        struct sig_click *c = args;
        lua_createtable(L, 0, 3);
        lua_pushinteger(L, c->button); lua_setfield(L, -2, "button");
        lua_pushinteger(L, c->x); lua_setfield(L, -2, "x");
        lua_pushinteger(L, c->y); lua_setfield(L, -2, "y");
    } else if (strcmp(name, "key") == 0 || strcmp(name, "keyup") == 0) {
        struct sig_key *k = args;
        lua_createtable(L, 0, 4);
        lua_pushinteger(L, k->code); lua_setfield(L, -2, "code");
        lua_pushinteger(L, k->ch); lua_setfield(L, -2, "ch");
        lua_pushinteger(L, k->mods); lua_setfield(L, -2, "mods");
        if (k->ch > 0) {
            lua_pushfstring(L, "%U", (long)k->ch);
            lua_setfield(L, -2, "char");
        }
    } else if (strcmp(name, "paint") == 0) {
        gui_push_painter(L, ((struct sig_paint *)args)->p);
    } else if (strcmp(name, "resize") == 0) {
        struct sig_resize *r = args;
        lua_createtable(L, 0, 2);
        lua_pushinteger(L, r->w); lua_setfield(L, -2, "w");
        lua_pushinteger(L, r->h); lua_setfield(L, -2, "h");
    } else if (strcmp(name, "scrolled") == 0) {
        lua_createtable(L, 0, 1);
        lua_pushinteger(L, ((struct sig_scroll *)args)->value); lua_setfield(L, -2, "value");
    } else if (strcmp(name, "selected") == 0 ||
               ((strcmp(name, "changed") == 0 || strcmp(name, "activate") == 0) &&
                (strcmp(cls, "combobox") == 0 || strcmp(cls, "tabs") == 0 ||
                 strcmp(cls, "listview") == 0 || strcmp(cls, "treeview") == 0 ||
                 strcmp(cls, "table") == 0))) {
        lua_createtable(L, 0, 1);
        lua_pushinteger(L, ((struct sig_select *)args)->index + 1); lua_setfield(L, -2, "index");
    } else if (strcmp(name, "changed") == 0 || strcmp(name, "toggled") == 0 ||
               strcmp(name, "activate") == 0 || strcmp(name, "focus") == 0) {
        struct sig_change *c = args;
        lua_createtable(L, 0, 2);
        lua_pushinteger(L, c->value); lua_setfield(L, -2, "value");
        if (c->text) {
            lua_pushstring(L, c->text);
            lua_setfield(L, -2, "text");
        }
    } else {
        lua_pushnil(L);
    }
    return 1;
}

/* The libgui handler of every connected signal: arg is the interned
 * signal name. */
static int trampoline(struct widget *w, void *args, void *arg)
{
    const char *name = arg;
    lua_State *L = gui_L;
    int top = lua_gettop(L);
    gui_push_widget(L, w);
    push_handler_table(L, -1);
    lua_getfield(L, -1, name);
    if (!lua_isfunction(L, -1)) {
        lua_settop(L, top);
        return 0;
    }
    lua_remove(L, -2);
    lua_insert(L, -2);                  /* fn, widget */
    push_signal_args(L, w, name, args);
    int painted = strcmp(name, "paint") == 0;
    if (painted) {
        /* A copy below the call survives it, so the painter the handler
         * may have kept is invalidated afterwards. */
        lua_pushvalue(L, -1);
        lua_insert(L, top + 1);
    }
    int consumed = 0;
    if (gui_call(L, 2, 1))
        consumed = lua_toboolean(L, -1);
    if (painted)
        gui_painter_close(L, top + 1);
    lua_settop(L, top);
    return painted ? 1 : consumed;
}

static int w_on(lua_State *L)
{
    struct widget *w = gui_check_widget(L, 1);
    const char *name = luaL_checkstring(L, 2);
    luaL_checktype(L, 3, LUA_TFUNCTION);
    push_handler_table(L, 1);
    lua_getfield(L, -1, name);
    int connected = !lua_isnil(L, -1);
    lua_pop(L, 1);
    lua_pushvalue(L, 3);
    lua_setfield(L, -2, name);
    if (!connected)
        widget_connect(w, name, trampoline, strdup(name));
    lua_settop(L, 1);
    return 1;
}

/* ---- widget methods ---- */

static int w_text(lua_State *L)
{
    struct widget *w = gui_check_widget(L, 1);
    if (lua_isnoneornil(L, 2)) {
        const char *t = widget_text(w);
        if (t)
            lua_pushstring(L, t);
        else
            lua_pushnil(L);
        return 1;
    }
    widget_set_text(w, luaL_checkstring(L, 2));
    lua_settop(L, 1);
    return 1;
}

static int w_value(lua_State *L)
{
    struct widget *w = gui_check_widget(L, 1);
    if (lua_isnoneornil(L, 2)) {
        lua_pushinteger(L, w->value);
        return 1;
    }
    widget_set_value(w, (int)luaL_checkinteger(L, 2));
    lua_settop(L, 1);
    return 1;
}

static int w_range(lua_State *L)
{
    widget_set_range(gui_check_widget(L, 1), (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3));
    lua_settop(L, 1);
    return 1;
}

static int w_visible(lua_State *L)
{
    struct widget *w = gui_check_widget(L, 1);
    if (lua_isnone(L, 2)) {
        lua_pushboolean(L, w->visible);
        return 1;
    }
    widget_set_visible(w, lua_toboolean(L, 2));
    lua_settop(L, 1);
    return 1;
}

static int w_enabled(lua_State *L)
{
    struct widget *w = gui_check_widget(L, 1);
    if (lua_isnone(L, 2)) {
        lua_pushboolean(L, w->enabled);
        return 1;
    }
    widget_set_enabled(w, lua_toboolean(L, 2));
    lua_settop(L, 1);
    return 1;
}

static int w_hint(lua_State *L)
{
    widget_set_hint(gui_check_widget(L, 1), (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3));
    lua_settop(L, 1);
    return 1;
}

static int w_min(lua_State *L)
{
    widget_set_min(gui_check_widget(L, 1), (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3));
    lua_settop(L, 1);
    return 1;
}

static int w_max(lua_State *L)
{
    widget_set_max(gui_check_widget(L, 1), (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3));
    lua_settop(L, 1);
    return 1;
}

static int w_stretch(lua_State *L)
{
    widget_set_stretch(gui_check_widget(L, 1), (int)luaL_checkinteger(L, 2), (int)luaL_optinteger(L, 3, lua_tointeger(L, 2)));
    lua_settop(L, 1);
    return 1;
}

static const char *const align_names[] = { "fill", "start", "center", "end", NULL };

static int w_align(lua_State *L)
{
    int x = luaL_checkoption(L, 2, NULL, align_names);
    int y = luaL_checkoption(L, 3, align_names[x], align_names);
    widget_set_align(gui_check_widget(L, 1), (enum align)x, (enum align)y);
    lua_settop(L, 1);
    return 1;
}

static int w_grid(lua_State *L)
{
    widget_set_grid(gui_check_widget(L, 1), (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3),
                    (int)luaL_optinteger(L, 4, 1), (int)luaL_optinteger(L, 5, 1));
    lua_settop(L, 1);
    return 1;
}

static int w_gridstretch(lua_State *L)
{
    grid_set_stretch(gui_check_widget(L, 1), (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3),
                     (int)luaL_checkinteger(L, 4));
    lua_settop(L, 1);
    return 1;
}

static int w_accel(lua_State *L)
{
    widget_set_accel(gui_check_widget(L, 1), (int)luaL_checkinteger(L, 2), (int)luaL_optinteger(L, 3, 0));
    lua_settop(L, 1);
    return 1;
}

static int w_padding(lua_State *L)
{
    widget_set_padding(gui_check_widget(L, 1), (int)luaL_checkinteger(L, 2));
    lua_settop(L, 1);
    return 1;
}

static int w_tip(lua_State *L)
{
    widget_set_tip(gui_check_widget(L, 1), luaL_checkstring(L, 2));
    lua_settop(L, 1);
    return 1;
}

static int w_id(lua_State *L)
{
    struct widget *w = gui_check_widget(L, 1);
    if (lua_isnoneornil(L, 2)) {
        if (w->id)
            lua_pushstring(L, w->id);
        else
            lua_pushnil(L);
        return 1;
    }
    widget_set_id(w, luaL_checkstring(L, 2));
    lua_settop(L, 1);
    return 1;
}

static int w_find(lua_State *L)
{
    gui_push_widget(L, widget_find(gui_check_widget(L, 1), luaL_checkstring(L, 2)));
    return 1;
}

static int w_class(lua_State *L)
{
    lua_pushstring(L, gui_check_widget(L, 1)->cls->name);
    return 1;
}

static int w_invalidate(lua_State *L)
{
    widget_invalidate(gui_check_widget(L, 1));
    return 0;
}

static int w_relayout(lua_State *L)
{
    widget_relayout(gui_check_widget(L, 1));
    return 0;
}

static int w_focus(lua_State *L)
{
    widget_focus(gui_check_widget(L, 1));
    return 0;
}

static int w_focused(lua_State *L)
{
    lua_pushboolean(L, gui_check_widget(L, 1)->focused);
    return 1;
}

static int w_capture(lua_State *L)
{
    widget_capture(gui_check_widget(L, 1));
    return 0;
}

static int w_size(lua_State *L)
{
    struct widget *w = gui_check_widget(L, 1);
    lua_pushinteger(L, w->w);
    lua_pushinteger(L, w->h);
    return 2;
}

static int w_pos(lua_State *L)
{
    struct widget *w = gui_check_widget(L, 1);
    lua_pushinteger(L, w->x);
    lua_pushinteger(L, w->y);
    return 2;
}

static int w_parent(lua_State *L)
{
    gui_push_widget(L, gui_check_widget(L, 1)->parent);
    return 1;
}

static int w_window(lua_State *L)
{
    gui_push_widget(L, gui_check_widget(L, 1)->window);
    return 1;
}

static int w_destroy(lua_State *L)
{
    widget_destroy(gui_check_widget(L, 1));
    return 0;
}

static int w_close(lua_State *L)
{
    struct widget *w = gui_check_widget(L, 1);
    if (strcmp(w->cls->name, "window") != 0)
        return luaL_error(L, "close: not a window");
    window_close(w);
    return 0;
}

static int w_title(lua_State *L)
{
    struct widget *w = gui_check_widget(L, 1);
    if (strcmp(w->cls->name, "window") != 0)
        return luaL_error(L, "title: not a window");
    widget_set_text(w, luaL_checkstring(L, 2));
    gui_set_title(window_state_of(w)->win, luaL_checkstring(L, 2));
    lua_settop(L, 1);
    return 1;
}

/* Items of list views and combo boxes. */
static int w_add(lua_State *L)
{
    struct widget *w = gui_check_widget(L, 1);
    const char *item = luaL_checkstring(L, 2);
    if (strcmp(w->cls->name, "combobox") == 0)
        combobox_add(w, item);
    else if (strcmp(w->cls->name, "listview") == 0)
        listview_add(w, item);
    else
        return luaL_error(L, "add: %s has no items", w->cls->name);
    lua_settop(L, 1);
    return 1;
}

static int w_clear(lua_State *L)
{
    struct widget *w = gui_check_widget(L, 1);
    if (strcmp(w->cls->name, "combobox") == 0)
        combobox_clear(w);
    else if (strcmp(w->cls->name, "listview") == 0)
        listview_clear(w);
    else
        return luaL_error(L, "clear: %s has no items", w->cls->name);
    return 0;
}

static int w_count(lua_State *L)
{
    struct widget *w = gui_check_widget(L, 1);
    if (strcmp(w->cls->name, "listview") != 0)
        return luaL_error(L, "count: %s has no items", w->cls->name);
    lua_pushinteger(L, listview_count(w));
    return 1;
}

static int w_item(lua_State *L)
{
    struct widget *w = gui_check_widget(L, 1);
    int index = (int)luaL_checkinteger(L, 2) - 1;
    const char *item;
    if (strcmp(w->cls->name, "combobox") == 0)
        item = combobox_item(w, index);
    else if (strcmp(w->cls->name, "listview") == 0)
        item = index >= 0 && index < listview_count(w) ? listview_item(w, index) : NULL;
    else
        return luaL_error(L, "item: %s has no items", w->cls->name);
    if (item)
        lua_pushstring(L, item);
    else
        lua_pushnil(L);
    return 1;
}

static int w_select(lua_State *L)
{
    struct widget *w = gui_check_widget(L, 1);
    int index = (int)luaL_checkinteger(L, 2) - 1;
    if (strcmp(w->cls->name, "combobox") == 0)
        combobox_select(w, index);
    else if (strcmp(w->cls->name, "tabs") == 0)
        tabs_select(w, index);
    else
        widget_set_value(w, index);
    lua_settop(L, 1);
    return 1;
}

static int w_page(lua_State *L)
{
    struct widget *w = gui_check_widget(L, 1);
    if (strcmp(w->cls->name, "tabs") != 0)
        return luaL_error(L, "page: not a tabs widget");
    gui_push_widget(L, tabs_add(w, luaL_checkstring(L, 2)));
    return 1;
}

static int w_position(lua_State *L)
{
    struct widget *w = gui_check_widget(L, 1);
    if (strcmp(w->cls->name, "splitpane") != 0)
        return luaL_error(L, "position: not a split pane");
    splitpane_set_position(w, (int)luaL_checkinteger(L, 2));
    lua_settop(L, 1);
    return 1;
}

static int w_set(lua_State *L)
{
    struct widget *w = gui_check_widget(L, 1);
    if (strcmp(w->cls->name, "scrollbar") != 0)
        return luaL_error(L, "set: not a scroll bar");
    scrollbar_set(w, (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3), (int)luaL_checkinteger(L, 4));
    lua_settop(L, 1);
    return 1;
}

static int w_tostring(lua_State *L)
{
    struct wref *r = luaL_checkudata(L, 1, GUI_WIDGET_META);
    if (r->w)
        lua_pushfstring(L, "%s: %p", r->w->cls->name, (void *)r->w);
    else
        lua_pushstring(L, "widget: destroyed");
    return 1;
}

static const luaL_Reg widget_methods[] = {
    { "on", w_on }, { "text", w_text }, { "value", w_value }, { "range", w_range },
    { "visible", w_visible }, { "enabled", w_enabled }, { "hint", w_hint }, { "min", w_min },
    { "max", w_max }, { "stretch", w_stretch }, { "align", w_align }, { "grid", w_grid },
    { "gridstretch", w_gridstretch }, { "accel", w_accel }, { "padding", w_padding },
    { "tip", w_tip }, { "id", w_id }, { "find", w_find }, { "class", w_class },
    { "invalidate", w_invalidate }, { "relayout", w_relayout }, { "focus", w_focus },
    { "focused", w_focused }, { "capture", w_capture }, { "size", w_size }, { "pos", w_pos },
    { "parent", w_parent }, { "window", w_window }, { "destroy", w_destroy },
    { "close", w_close }, { "title", w_title }, { "add", w_add }, { "clear", w_clear },
    { "count", w_count }, { "item", w_item }, { "select", w_select }, { "page", w_page },
    { "position", w_position }, { "set", w_set },
    { "__tostring", w_tostring },
    { NULL, NULL }
};

/* ---- constructors ---- */

#define CONSTRUCTOR(name, call) \
    static int g_##name(lua_State *L) \
    { \
        struct widget *parent = gui_check_widget(L, 1); \
        gui_push_widget(L, call); \
        return 1; \
    }

CONSTRUCTOR(box, box_new(parent, lua_toboolean(L, 2)))
CONSTRUCTOR(vbox, box_new(parent, 1))
CONSTRUCTOR(hbox, box_new(parent, 0))
CONSTRUCTOR(grid, grid_new(parent))
CONSTRUCTOR(label, label_new(parent, luaL_optstring(L, 2, "")))
CONSTRUCTOR(button, button_new(parent, luaL_checkstring(L, 2)))
CONSTRUCTOR(checkbox, checkbox_new(parent, luaL_checkstring(L, 2)))
CONSTRUCTOR(radio, radio_new(parent, luaL_checkstring(L, 2)))
CONSTRUCTOR(textfield, textfield_new(parent, luaL_optstring(L, 2, "")))
CONSTRUCTOR(canvas, canvas_new(parent))
CONSTRUCTOR(separator, separator_new(parent))
CONSTRUCTOR(listview, listview_new(parent))
CONSTRUCTOR(scrollbar, scrollbar_new(parent, lua_toboolean(L, 2)))
CONSTRUCTOR(scrollarea, scrollarea_new(parent))
CONSTRUCTOR(combobox, combobox_new(parent))
CONSTRUCTOR(spinner, spinner_new(parent, (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3), (int)luaL_optinteger(L, 4, lua_tointeger(L, 2))))
CONSTRUCTOR(slider, slider_new(parent, (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3), (int)luaL_optinteger(L, 4, lua_tointeger(L, 2))))
CONSTRUCTOR(progress, progress_new(parent))
CONSTRUCTOR(tabs, tabs_new(parent))
CONSTRUCTOR(splitpane, splitpane_new(parent, lua_toboolean(L, 2)))

/* ---- application ---- */

static struct app *check_app(lua_State *L, int index)
{
    struct aref *r = luaL_checkudata(L, index, GUI_APP_META);
    if (!r->a)
        luaL_error(L, "application was destroyed");
    return r->a;
}

static int g_app(lua_State *L)
{
#ifdef MINIOS_HOST
    struct app *a = app_create_detached();
#else
    struct app *a = app_create();
#endif
    if (!a) {
        lua_pushnil(L);
        lua_pushstring(L, "cannot connect to the display server");
        return 2;
    }
    struct aref *r = lua_newuserdatauv(L, sizeof *r, 0);
    r->a = a;
    luaL_setmetatable(L, GUI_APP_META);
    return 1;
}

static int a_window(lua_State *L)
{
    struct app *a = check_app(L, 1);
    gui_push_widget(L, app_window(a, (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3),
                                  luaL_optstring(L, 4, "")));
    return 1;
}

static int a_modal(lua_State *L)
{
    struct app *a = check_app(L, 1);
    struct widget *parent = gui_check_widget(L, 2);
    gui_push_widget(L, app_modal_window(a, parent, (int)luaL_checkinteger(L, 3), (int)luaL_checkinteger(L, 4),
                                        luaL_optstring(L, 5, "")));
    return 1;
}

static int a_run(lua_State *L)
{
    lua_pushinteger(L, app_run(check_app(L, 1)));
    return 1;
}

static int a_quit(lua_State *L)
{
    app_quit(check_app(L, 1), (int)luaL_optinteger(L, 2, 0));
    return 0;
}

static int a_step(lua_State *L)
{
    lua_pushboolean(L, app_step(check_app(L, 1), (int)luaL_optinteger(L, 2, -1)));
    return 1;
}

static void timer_fire(void *arg)
{
    struct tref *t = arg;
    lua_State *L = gui_L;
    int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, t->self);
    lua_getiuservalue(L, -1, 1);
    lua_pushvalue(L, -2);
    if (!t->repeat) {
        t->t = NULL;                    /* libgui freed the timer before the call */
        luaL_unref(L, LUA_REGISTRYINDEX, t->self);
        t->self = LUA_NOREF;
    }
    gui_call(L, 1, 0);
    lua_settop(L, top);
}

/* app:timer(ms, repeat, fn): fn(timer) runs after ms milliseconds, and
 * every ms milliseconds when repeat is true. */
static int a_timer(lua_State *L)
{
    struct app *a = check_app(L, 1);
    int ms = (int)luaL_checkinteger(L, 2);
    int repeat = lua_toboolean(L, 3);
    luaL_checktype(L, 4, LUA_TFUNCTION);
    struct tref *t = lua_newuserdatauv(L, sizeof *t, 1);
    t->a = a;
    t->repeat = repeat;
    lua_pushvalue(L, 4);
    lua_setiuservalue(L, -2, 1);
    luaL_setmetatable(L, GUI_TIMER_META);
    lua_pushvalue(L, -1);
    t->self = luaL_ref(L, LUA_REGISTRYINDEX);
    t->t = app_timer_add(a, ms, repeat, timer_fire, t);
    return 1;
}

static int t_remove(lua_State *L)
{
    struct tref *t = luaL_checkudata(L, 1, GUI_TIMER_META);
    if (t->t) {
        app_timer_remove(t->a, t->t);
        t->t = NULL;
    }
    if (t->self != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, t->self);
        t->self = LUA_NOREF;
    }
    return 0;
}

static void watch_fire(int fd, int revents, void *arg)
{
    struct href *h = arg;
    lua_State *L = gui_L;
    int top = lua_gettop(L);
    lua_rawgeti(L, LUA_REGISTRYINDEX, h->self);
    lua_getiuservalue(L, -1, 1);
    lua_pushinteger(L, fd);
    lua_pushstring(L, revents & POLLIN ? (revents & POLLOUT ? "rw" : "r") : (revents & POLLOUT ? "w" : ""));
    gui_call(L, 2, 0);
    lua_settop(L, top);
}

/* app:watch(fd, "r"|"w"|"rw", fn): fn(fd, ready) runs when fd is ready. */
static int a_watch(lua_State *L)
{
    struct app *a = check_app(L, 1);
    int fd = (int)luaL_checkinteger(L, 2);
    const char *events = luaL_checkstring(L, 3);
    luaL_checktype(L, 4, LUA_TFUNCTION);
    int mask = (strchr(events, 'r') ? POLLIN : 0) | (strchr(events, 'w') ? POLLOUT : 0);
    struct href *h = lua_newuserdatauv(L, sizeof *h, 1);
    h->a = a;
    lua_pushvalue(L, 4);
    lua_setiuservalue(L, -2, 1);
    luaL_setmetatable(L, GUI_WATCH_META);
    lua_pushvalue(L, -1);
    h->self = luaL_ref(L, LUA_REGISTRYINDEX);
    h->h = app_watch_fd(a, fd, mask, watch_fire, h);
    return 1;
}

static int h_remove(lua_State *L)
{
    struct href *h = luaL_checkudata(L, 1, GUI_WATCH_META);
    if (h->h) {
        app_unwatch_fd(h->a, h->h);
        h->h = NULL;
    }
    if (h->self != LUA_NOREF) {
        luaL_unref(L, LUA_REGISTRYINDEX, h->self);
        h->self = LUA_NOREF;
    }
    return 0;
}

static const char *const color_names[TC_COUNT] = {
    "window", "text", "text_disabled", "field", "selection", "selection_text",
    "accent", "border", "highlight", "button", "button_pressed", "track", "thumb",
    "button_hover",
};
static const char *const metric_names[TM_COUNT] = {
    "padding", "spacing", "border", "radius", "scrollbar", "font_px", "control_h",
};

/* app:theme(): the colours by name, the scaled metrics by name and the
 * scale in percent. */
static int a_theme(lua_State *L)
{
    struct theme *t = app_theme(check_app(L, 1));
    lua_createtable(L, 0, 3);
    lua_createtable(L, 0, TC_COUNT);
    for (int i = 0; i < TC_COUNT; i++) {
        lua_pushinteger(L, t->color[i]);
        lua_setfield(L, -2, color_names[i]);
    }
    lua_setfield(L, -2, "color");
    lua_createtable(L, 0, TM_COUNT);
    for (int i = 0; i < TM_COUNT; i++) {
        lua_pushinteger(L, theme_px(t, (enum theme_metric)i));
        lua_setfield(L, -2, metric_names[i]);
    }
    lua_setfield(L, -2, "metric");
    lua_pushinteger(L, t->scale);
    lua_setfield(L, -2, "scale");
    return 1;
}

/* app:dialog(title, text, {buttons}): the index of the chosen button. */
static int a_dialog(lua_State *L)
{
    struct app *a = check_app(L, 1);
    const char *title = luaL_checkstring(L, 2), *text = luaL_checkstring(L, 3);
    luaL_checktype(L, 4, LUA_TTABLE);
    const char *buttons[8];
    int n = 0;
    for (; n < 8; n++) {
        lua_rawgeti(L, 4, n + 1);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            break;
        }
        buttons[n] = luaL_checkstring(L, -1);
        lua_pop(L, 1);
    }
    int r = app_dialog(a, title, text, buttons, n);
    if (r < 0)
        lua_pushnil(L);
    else
        lua_pushinteger(L, r + 1);
    return 1;
}

/* app:prompt(title, label [, default]): the entered text or nil. */
static int a_prompt(lua_State *L)
{
    struct app *a = check_app(L, 1);
    char buf[256];
    strlcpy(buf, luaL_optstring(L, 4, ""), sizeof buf);
    if (app_prompt(a, luaL_checkstring(L, 2), luaL_checkstring(L, 3), buf, sizeof buf))
        lua_pushstring(L, buf);
    else
        lua_pushnil(L);
    return 1;
}

static int a_destroy(lua_State *L)
{
    struct aref *r = luaL_checkudata(L, 1, GUI_APP_META);
    if (r->a)
        app_destroy(r->a);
    r->a = NULL;
    return 0;
}

static const luaL_Reg app_methods[] = {
    { "window", a_window }, { "modal", a_modal }, { "run", a_run }, { "quit", a_quit },
    { "step", a_step }, { "timer", a_timer }, { "watch", a_watch }, { "theme", a_theme },
    { "dialog", a_dialog }, { "prompt", a_prompt }, { "destroy", a_destroy },
    { NULL, NULL }
};

/* ---- test helpers: messages injected into a window ---- */

static int test_key(lua_State *L)
{
    struct widget *win = gui_check_widget(L, 1);
    struct wmsg m = { .type = WM_KEY, .window = window_state_of(win)->win->id,
                      .a = (int32_t)luaL_checkinteger(L, 2), .b = lua_toboolean(L, 5) ? 0 : 1,
                      .c = (int32_t)luaL_optinteger(L, 4, 0), .d = (int32_t)luaL_optinteger(L, 3, 0) };
    window_message(win, &m);
    return 0;
}

static const char *const mouse_kinds[] = { "move", "down", "up", "wheel", NULL };

static int test_mouse(lua_State *L)
{
    struct widget *win = gui_check_widget(L, 1);
    struct wmsg m = { .type = WM_MOUSE, .window = window_state_of(win)->win->id,
                      .a = (int32_t)luaL_checkinteger(L, 3), .b = (int32_t)luaL_checkinteger(L, 4),
                      .c = (int32_t)luaL_optinteger(L, 5, 0), .d = luaL_checkoption(L, 2, NULL, mouse_kinds) };
    window_message(win, &m);
    return 0;
}

static int test_paint(lua_State *L)
{
    struct rect r = window_paint(gui_check_widget(L, 1));
    lua_pushinteger(L, r.x);
    lua_pushinteger(L, r.y);
    lua_pushinteger(L, r.w);
    lua_pushinteger(L, r.h);
    return 4;
}

static int test_pixel(lua_State *L)
{
    struct widget *win = gui_check_widget(L, 1);
    struct gui_window *g = window_state_of(win)->win;
    int x = (int)luaL_checkinteger(L, 2) * g->scale, y = (int)luaL_checkinteger(L, 3) * g->scale;
    if (x < 0 || y < 0 || x >= g->surf.width || y >= g->surf.height)
        return luaL_error(L, "pixel outside the window");
    lua_pushinteger(L, g->surf.pixels[y * g->surf.stride + x] & 0x00ffffff);
    return 1;
}

static int test_close(lua_State *L)
{
    struct widget *win = gui_check_widget(L, 1);
    struct wmsg m = { .type = WM_CLOSE, .window = window_state_of(win)->win->id };
    window_message(win, &m);
    return 0;
}

static const luaL_Reg test_funcs[] = {
    { "key", test_key }, { "mouse", test_mouse }, { "paint", test_paint }, { "pixel", test_pixel },
    { "close", test_close },
    { NULL, NULL }
};

static const luaL_Reg gui_funcs[] = {
    { "app", g_app },
    { "box", g_box }, { "vbox", g_vbox }, { "hbox", g_hbox }, { "grid", g_grid },
    { "label", g_label }, { "button", g_button }, { "checkbox", g_checkbox }, { "radio", g_radio },
    { "textfield", g_textfield }, { "canvas", g_canvas }, { "separator", g_separator },
    { "listview", g_listview }, { "scrollbar", g_scrollbar }, { "scrollarea", g_scrollarea },
    { "combobox", g_combobox }, { "spinner", g_spinner }, { "slider", g_slider },
    { "progress", g_progress }, { "tabs", g_tabs }, { "splitpane", g_splitpane },
    { NULL, NULL }
};

static void new_meta(lua_State *L, const char *name, const luaL_Reg *methods)
{
    luaL_newmetatable(L, name);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, methods, 0);
    lua_pop(L, 1);
}

static const luaL_Reg timer_methods[] = { { "remove", t_remove }, { NULL, NULL } };
static const luaL_Reg watch_methods[] = { { "remove", h_remove }, { NULL, NULL } };

int luaopen_gui(lua_State *L)
{
    lua_rawgeti(L, LUA_REGISTRYINDEX, LUA_RIDX_MAINTHREAD);
    gui_L = lua_tothread(L, -1);
    lua_pop(L, 1);
    lua_newtable(L);
    lua_rawsetp(L, LUA_REGISTRYINDEX, &widgets_key);
    new_meta(L, GUI_WIDGET_META, widget_methods);
    new_meta(L, GUI_APP_META, app_methods);
    new_meta(L, GUI_TIMER_META, timer_methods);
    new_meta(L, GUI_WATCH_META, watch_methods);
    gui_open_painter(L);
    luaL_newlib(L, gui_funcs);
    luaL_newlib(L, test_funcs);
    lua_setfield(L, -2, "test");
    gui_push_constants(L);
    return 1;
}
