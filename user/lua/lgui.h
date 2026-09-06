#pragma once
/* Shared between lgui.c (widgets, application) and lpaint.c (painter,
 * constants). */
#include <gui/app.h>
#include "lua.h"

#define GUI_WIDGET_META "gui.widget"
#define GUI_APP_META "gui.app"
#define GUI_TIMER_META "gui.timer"
#define GUI_WATCH_META "gui.watch"
#define GUI_PAINTER_META "gui.painter"

void gui_push_widget(lua_State *L, struct widget *w);
struct widget *gui_check_widget(lua_State *L, int index);
/* Calls the function under nargs arguments; prints an error with its
 * traceback and returns 0, or returns nresults. */
int gui_call(lua_State *L, int nargs, int nresults);

/* lpaint.c */
void gui_open_painter(lua_State *L);
void gui_push_painter(lua_State *L, struct painter *p);
/* Invalidates the painter userdata at index once the paint returned. */
void gui_painter_close(lua_State *L, int index);
/* Adds key, mod and rgb to the module table on top of the stack. */
void gui_push_constants(lua_State *L);
