/* The painter of a paint signal, the key and modifier tables and the
 * colour helper of the gui module. A painter is valid during the paint
 * handler only: the pointer is cleared when the handler returns. */
#include <string.h>
#include <minios/input.h>
#include "lauxlib.h"
#include "lgui.h"

struct pref { struct painter *p; };

void gui_push_painter(lua_State *L, struct painter *p)
{
    struct pref *r = lua_newuserdatauv(L, sizeof *r, 0);
    r->p = p;
    luaL_setmetatable(L, GUI_PAINTER_META);
}

void gui_painter_close(lua_State *L, int index)
{
    struct pref *r = luaL_testudata(L, index, GUI_PAINTER_META);
    if (r)
        r->p = NULL;
}

static struct painter *check_painter(lua_State *L, int index)
{
    struct pref *r = luaL_checkudata(L, index, GUI_PAINTER_META);
    if (!r->p)
        luaL_error(L, "painter used outside its paint handler");
    return r->p;
}

static uint32_t color_arg(lua_State *L, int index)
{
    return (uint32_t)luaL_checkinteger(L, index) & 0x00ffffff;
}

static int p_fill(lua_State *L)
{
    painter_fill(check_painter(L, 1), (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3),
                 (int)luaL_checkinteger(L, 4), (int)luaL_checkinteger(L, 5), color_arg(L, 6));
    return 0;
}

static int p_frame(lua_State *L)
{
    painter_frame(check_painter(L, 1), (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3),
                  (int)luaL_checkinteger(L, 4), (int)luaL_checkinteger(L, 5), color_arg(L, 6));
    return 0;
}

static int p_line(lua_State *L)
{
    painter_line(check_painter(L, 1), (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3),
                 (int)luaL_checkinteger(L, 4), (int)luaL_checkinteger(L, 5), color_arg(L, 6));
    return 0;
}

static int p_rounded(lua_State *L)
{
    painter_rounded(check_painter(L, 1), (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3),
                    (int)luaL_checkinteger(L, 4), (int)luaL_checkinteger(L, 5), color_arg(L, 6), color_arg(L, 7));
    return 0;
}

static int p_text(lua_State *L)
{
    painter_text(check_painter(L, 1), (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3),
                 luaL_checkstring(L, 4), color_arg(L, 5));
    return 0;
}

static int p_text_width(lua_State *L)
{
    size_t len;
    const char *s = luaL_checklstring(L, 2, &len);
    lua_pushinteger(L, painter_text_width(check_painter(L, 1), s, (int)len));
    return 1;
}

static int p_text_height(lua_State *L)
{
    lua_pushinteger(L, painter_text_height(check_painter(L, 1)));
    return 1;
}

static int p_push(lua_State *L)
{
    painter_push(check_painter(L, 1), (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3),
                 (int)luaL_checkinteger(L, 4), (int)luaL_checkinteger(L, 5));
    return 0;
}

static int p_pop(lua_State *L)
{
    painter_pop(check_painter(L, 1));
    return 0;
}

static int p_focus_ring(lua_State *L)
{
    painter_focus_ring(check_painter(L, 1), (int)luaL_checkinteger(L, 2), (int)luaL_checkinteger(L, 3),
                       (int)luaL_checkinteger(L, 4), (int)luaL_checkinteger(L, 5));
    return 0;
}

static int p_clip(lua_State *L)
{
    struct rect r = painter_clip_local(check_painter(L, 1));
    lua_pushinteger(L, r.x);
    lua_pushinteger(L, r.y);
    lua_pushinteger(L, r.w);
    lua_pushinteger(L, r.h);
    return 4;
}

/* p:image(img, x, y [, w, h]) draws the image at its own size, or
 * resampled to w by h logical pixels. */
static int p_image(lua_State *L)
{
    struct painter *p = check_painter(L, 1);
    const struct image *img = gui_check_image(L, 2);
    int x = (int)luaL_checkinteger(L, 3), y = (int)luaL_checkinteger(L, 4);
    if (lua_isnoneornil(L, 5)) {
        painter_image(p, x, y, img);
        return 0;
    }
    int w = (int)luaL_checkinteger(L, 5), h = (int)luaL_checkinteger(L, 6);
    if (w <= 0 || h <= 0)
        return 0;
    const struct image *sized = gui_image_sized(L, 2, w, h, p->scale);
    if (!sized)
        return luaL_error(L, "not enough memory");
    painter_image(p, x, y, sized);
    return 0;
}

static const luaL_Reg painter_methods[] = {
    { "fill", p_fill }, { "frame", p_frame }, { "line", p_line }, { "rounded", p_rounded },
    { "text", p_text }, { "text_width", p_text_width }, { "text_height", p_text_height },
    { "push", p_push }, { "pop", p_pop }, { "focus_ring", p_focus_ring }, { "clip", p_clip },
    { "image", p_image },
    { NULL, NULL }
};

void gui_open_painter(lua_State *L)
{
    luaL_newmetatable(L, GUI_PAINTER_META);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    luaL_setfuncs(L, painter_methods, 0);
    lua_pop(L, 1);
}

/* gui.rgb(r, g, b): a colour integer. */
static int g_rgb(lua_State *L)
{
    lua_pushinteger(L, gfx_rgb((int)luaL_checkinteger(L, 1), (int)luaL_checkinteger(L, 2),
                               (int)luaL_checkinteger(L, 3)));
    return 1;
}

static const struct { const char *name; int code; } keys[] = {
    { "esc", KEY_ESC }, { "backspace", KEY_BACKSPACE }, { "tab", KEY_TAB }, { "enter", KEY_ENTER },
    { "space", KEY_SPACE }, { "up", KEY_UP }, { "down", KEY_DOWN }, { "left", KEY_LEFT },
    { "right", KEY_RIGHT }, { "home", KEY_HOME }, { "end", KEY_END }, { "pageup", KEY_PAGEUP },
    { "pagedown", KEY_PAGEDOWN }, { "delete", KEY_DELETE },
    { "f1", KEY_F1 }, { "f2", KEY_F2 }, { "f3", KEY_F3 }, { "f4", KEY_F4 }, { "f5", KEY_F5 },
    { "f6", KEY_F6 }, { "f7", KEY_F7 }, { "f8", KEY_F8 }, { "f9", KEY_F9 }, { "f10", KEY_F10 },
    { "f11", KEY_F11 }, { "f12", KEY_F12 },
};

/* The letter and digit rows of the Linux key codes. */
static const char *const rows[] = { "1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm" };
static const int row_codes[] = { 2, 16, 30, 44 };

void gui_push_constants(lua_State *L)
{
    lua_createtable(L, 0, 64);
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        lua_pushinteger(L, keys[i].code);
        lua_setfield(L, -2, keys[i].name);
    }
    for (int r = 0; r < 4; r++) {
        for (int i = 0; rows[r][i]; i++) {
            lua_pushinteger(L, row_codes[r] + i);
            lua_pushlstring(L, &rows[r][i], 1);
            lua_insert(L, -2);
            lua_settable(L, -3);
        }
    }
    lua_setfield(L, -2, "key");
    lua_createtable(L, 0, 4);
    lua_pushinteger(L, WMOD_SHIFT); lua_setfield(L, -2, "shift");
    lua_pushinteger(L, WMOD_CTRL); lua_setfield(L, -2, "ctrl");
    lua_pushinteger(L, WMOD_ALT); lua_setfield(L, -2, "alt");
    lua_pushinteger(L, WMOD_LOGO); lua_setfield(L, -2, "logo");
    lua_setfield(L, -2, "mod");
    lua_pushcfunction(L, g_rgb);
    lua_setfield(L, -2, "rgb");
}
