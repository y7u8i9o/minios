/* The mime module, /usr/lib/lua/5.5/mime.so: the type of a file, the
 * program registered for a type and the opening of a file with it, from
 * the tables of libgui (gui/mime.h, docs/design/desktop.md). It is a
 * module of its own rather than part of gui, because worker threads may
 * use it and gui belongs to the main thread. Failures return nil, the
 * message and the errno, like io.open. */
#include <errno.h>
#include <sys/stat.h>
#include <gui/mime.h>
#include "lauxlib.h"
#include "minios.h"

/* mime.open(path): starts the program registered for the file's type,
 * or the exec line of a launcher. */
static int mime_l_open(lua_State *L)
{
    int r = (int)mime_open(luaL_checkstring(L, 1));
    if (r < 0) {
        errno = -r;
        return minios_errresult(L);
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* mime.type(path): the MIME type of a file, inode/directory for a
 * directory. */
static int mime_l_type(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    struct stat st;
    int is_dir = stat(path, &st) == 0 && S_ISDIR(st.st_mode);
    lua_pushstring(L, mime_type(path, is_dir));
    return 1;
}

/* mime.load(types, apps): reads the tables from other paths. The host
 * unit test uses it, and programs on the target rely on the defaults. */
static int mime_l_load(lua_State *L)
{
    int r = mime_load(luaL_checkstring(L, 1), luaL_checkstring(L, 2));
    if (r < 0) {
        errno = -r;
        return minios_errresult(L);
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* mime.handler(type): the program registered for a type, or nil. */
static int mime_l_handler(lua_State *L)
{
    const char *prog = mime_handler(luaL_checkstring(L, 1));
    if (prog)
        lua_pushstring(L, prog);
    else
        lua_pushnil(L);
    return 1;
}

static const luaL_Reg mime_funcs[] = {
    { "open", mime_l_open },
    { "type", mime_l_type },
    { "handler", mime_l_handler },
    { "load", mime_l_load },
    { NULL, NULL }
};

int luaopen_mime(lua_State *L)
{
    luaL_newlib(L, mime_funcs);
    return 1;
}
