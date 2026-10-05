#pragma once
/* The minios modules of the Lua interpreter. fs, sys, thread and net are part
 * of the interpreter and registered in package.preload by linit.c. gui,
 * audio and mime are C modules in /usr/lib/lua/5.5, which require finds
 * through package.cpath and loads with dlopen. A module uses only Lua,
 * its libraries and what this header defines, never a symbol of the
 * interpreter program. */
#include <errno.h>
#include <string.h>
#include "lua.h"

int luaopen_fs(lua_State *L);       /* lfs.c: directories, file status */
int luaopen_sys(lua_State *L);      /* lsys.c: processes and system data */
int luaopen_thread(lua_State *L);   /* lthread.c: native workers and channels */
int luaopen_net(lua_State *L);      /* lnet.c: TCP sockets */
int luaopen_gui(lua_State *L);      /* lgui.c, lpaint.c, limage.c: the libgui application framework, gui.so */
int luaopen_audio(lua_State *L);    /* laudio.c: playback, capture and mixer, audio.so */
int luaopen_mime(lua_State *L);     /* lmime.c: MIME types and handlers, mime.so */
void minios_sys_extra(lua_State *L);

/* The registry field that stores the job of a worker state (lthread.c). */
#define MINIOS_WORKER_KEY "minios.worker"

/* True in the state of a worker thread. */
static inline int minios_lua_worker(lua_State *L)
{
    int worker = lua_getfield(L, LUA_REGISTRYINDEX, MINIOS_WORKER_KEY) != LUA_TNIL;
    lua_pop(L, 1);
    return worker;
}

/* Pushes nil, strerror(errno) and errno, the convention of the io
 * library, and returns 3. */
static inline int minios_errresult(lua_State *L)
{
    int err = errno;
    lua_pushnil(L);
    lua_pushstring(L, strerror(err));
    lua_pushinteger(L, err);
    return 3;
}
