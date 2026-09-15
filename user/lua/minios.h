#pragma once
/* The minios modules of the Lua interpreter. Each is registered in
 * package.preload by linit.c, so a script loads one with require. */
#include "lua.h"

int luaopen_fs(lua_State *L);       /* lfs.c: directories, file status */
int luaopen_sys(lua_State *L);      /* lsys.c: processes, MIME types, system data */
int luaopen_gui(lua_State *L);      /* lgui.c, lpaint.c: the libgui application framework */
int luaopen_audio(lua_State *L);    /* laudio.c: playback, capture and mixer */
int luaopen_thread(lua_State *L);   /* lthread.c: native workers and channels */
int minios_lua_worker(lua_State *L);
void minios_sys_extra(lua_State *L);

/* Pushes nil, strerror(errno) and errno, the convention of the io
 * library, and returns 3. */
int minios_errresult(lua_State *L);
