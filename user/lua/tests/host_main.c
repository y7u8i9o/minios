/* Host driver of the module test: runs a script with the arguments in
 * arg, as /bin/lua does, and exits with 1 on an error. */
#include <stdio.h>
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: test_modules script args...\n");
        return 2;
    }
    lua_State *L = luaL_newstate();
    luaL_openlibs(L);
    lua_createtable(L, argc - 2, 1);
    for (int i = 1; i < argc; i++) {
        lua_pushstring(L, argv[i]);
        lua_rawseti(L, -2, i - 1);
    }
    lua_setglobal(L, "arg");
    int status = luaL_dofile(L, argv[1]);
    if (status != LUA_OK)
        fprintf(stderr, "%s\n", lua_tostring(L, -1));
    lua_close(L);
    return status == LUA_OK ? 0 : 1;
}
