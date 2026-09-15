/* Lua ownership and PCM conversion over libaudio. Children retain their
 * connection as a user value, but the connection does not retain children.
 * Closing a connection frees its native children; every child checks the
 * owner's pointer before touching its own (now possibly stale) pointer. */
#include <audio/audio.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <string.h>
#include "lauxlib.h"
#include "minios.h"

#define CONNECTION "audio.connection"
#define PLAYBACK "audio.playback"
#define CAPTURE "audio.capture"
#define MIXER "audio.mixer"
#define FRAME_BYTES 4       /* interleaved stereo S16_LE */

struct connection_ref { struct audio_connection *ptr; };
struct child_ref { void *ptr; struct connection_ref *owner; };

static int success(lua_State *L, int result)
{
    if (result < 0)
        return minios_errresult(L);
    lua_pushboolean(L, 1);
    return 1;
}

static struct connection_ref *connection(lua_State *L)
{
    struct connection_ref *r = luaL_checkudata(L, 1, CONNECTION);
    luaL_argcheck(L, r->ptr != NULL, 1, "closed audio connection");
    return r;
}

static void *child(lua_State *L, const char *meta)
{
    struct child_ref *r = luaL_checkudata(L, 1, meta);
    luaL_argcheck(L, r->ptr && r->owner && r->owner->ptr, 1, "closed audio object");
    return r->ptr;
}

/* Allocate and register a finalizer before acquiring any native resource. */
static struct child_ref *new_child(lua_State *L, const char *meta,
                                    struct connection_ref *owner)
{
    struct child_ref *r = lua_newuserdatauv(L, sizeof *r, 1);
    r->ptr = NULL;
    r->owner = owner;
    luaL_setmetatable(L, meta);
    lua_pushvalue(L, 1);
    lua_setiuservalue(L, -2, 1);
    return r;
}

static int connection_close(lua_State *L)
{
    struct connection_ref *r = luaL_checkudata(L, 1, CONNECTION);
    audio_disconnect(r->ptr);
    r->ptr = NULL;
    return 0;
}

static int child_close(lua_State *L, const char *meta, void (*destroy)(void *))
{
    struct child_ref *r = luaL_checkudata(L, 1, meta);
    if (r->ptr && r->owner && r->owner->ptr)
        destroy(r->ptr);
    r->ptr = NULL;
    r->owner = NULL;
    lua_pushnil(L);
    lua_setiuservalue(L, 1, 1);
    return 0;
}

static void destroy_playback(void *p) { audio_playback_destroy(p); }
static void destroy_capture(void *p) { audio_capture_destroy(p); }
static void destroy_mixer(void *p) { audio_mixer_destroy(p); }
static int playback_close(lua_State *L) { return child_close(L, PLAYBACK, destroy_playback); }
static int capture_close(lua_State *L) { return child_close(L, CAPTURE, destroy_capture); }
static int mixer_close(lua_State *L) { return child_close(L, MIXER, destroy_mixer); }

static const char *stream_name(lua_State *L, const char *fallback)
{
    size_t len;
    const char *name = luaL_optlstring(L, 2, fallback, &len);
    luaL_argcheck(L, len < 48 && !memchr(name, 0, len), 2,
                  "stream name must be at most 47 bytes without NUL");
    return name;
}

static unsigned volume(lua_State *L, int index)
{
    lua_Integer n = luaL_checkinteger(L, index);
    luaL_argcheck(L, n >= 0 && n <= 200, index, "volume must be between 0 and 200");
    return (unsigned)n;
}

static int connect_audio(lua_State *L)
{
    struct connection_ref *r = lua_newuserdatauv(L, sizeof *r, 0);
    r->ptr = NULL;
    luaL_setmetatable(L, CONNECTION);
    r->ptr = audio_connect();
    return r->ptr ? 1 : minios_errresult(L);
}

static int connection_playback(lua_State *L)
{
    struct connection_ref *c = connection(L);
    const char *name = stream_name(L, "Lua playback");
    struct child_ref *r = new_child(L, PLAYBACK, c);
    r->ptr = audio_playback_create(c->ptr, name);
    return r->ptr ? 1 : minios_errresult(L);
}

static int connection_capture(lua_State *L)
{
    static const char *const sources[] = { "input", "monitor", NULL };
    struct connection_ref *c = connection(L);
    const char *name = stream_name(L, "Lua capture");
    int source = luaL_checkoption(L, 3, "input", sources);
    struct child_ref *r = new_child(L, CAPTURE, c);
    r->ptr = audio_capture_create(c->ptr, name,
        source == 0 ? AUDIO_SOURCE_INPUT : AUDIO_SOURCE_MONITOR);
    return r->ptr ? 1 : minios_errresult(L);
}

static int connection_mixer(lua_State *L)
{
    struct connection_ref *c = connection(L);
    struct child_ref *r = new_child(L, MIXER, c);
    r->ptr = audio_mixer_create(c->ptr);
    return r->ptr ? 1 : minios_errresult(L);
}

static int connection_fd(lua_State *L)
{
    lua_pushinteger(L, audio_connection_fd(connection(L)->ptr));
    return 1;
}

static int connection_dispatch(lua_State *L)
{
    struct connection_ref *c = connection(L);
    lua_Integer timeout = luaL_optinteger(L, 2, 0);
    luaL_argcheck(L, timeout >= -1 && timeout <= INT_MAX, 2, "invalid timeout");
    int n = audio_connection_dispatch(c->ptr, (int)timeout);
    if (n < 0)
        return minios_errresult(L);
    lua_pushinteger(L, n);
    return 1;
}

static int connection_sync(lua_State *L)
{
    return success(L, audio_connection_sync(connection(L)->ptr));
}

static int playback_write(lua_State *L)
{
    struct audio_playback *p = child(L, PLAYBACK);
    size_t bytes;
    luaL_checktype(L, 2, LUA_TSTRING);
    const char *pcm = lua_tolstring(L, 2, &bytes);
    luaL_argcheck(L, bytes % FRAME_BYTES == 0, 2, "PCM must contain whole stereo S16_LE frames");
    /* Lua userdata is aligned for int16_t; Lua strings need not be. */
    int16_t *samples = lua_newuserdatauv(L, bytes, 0);
    memcpy(samples, pcm, bytes);
    ssize_t n = audio_playback_write(p, samples, bytes / FRAME_BYTES);
    if (n < 0)
        return minios_errresult(L);
    lua_pushinteger(L, n);
    return 1;
}

static int capture_read(lua_State *L)
{
    struct audio_capture *p = child(L, CAPTURE);
    lua_Integer frames = luaL_optinteger(L, 2, audio_capture_quantum(p));
    luaL_argcheck(L, frames >= 0 && (lua_Unsigned)frames <= SIZE_MAX / FRAME_BYTES &&
                  frames <= LUA_MAXINTEGER / FRAME_BYTES, 2, "invalid frame count");
    int16_t *samples = lua_newuserdatauv(L, (size_t)frames * FRAME_BYTES, 0);
    ssize_t n = audio_capture_read(p, samples, (size_t)frames);
    if (n < 0)
        return minios_errresult(L);
    lua_pushlstring(L, (const char *)samples, (size_t)n * FRAME_BYTES);
    lua_pushinteger(L, n);
    return 2;
}

static int playback_start(lua_State *L) { return success(L, audio_playback_start(child(L, PLAYBACK))); }
static int playback_pause(lua_State *L) { return success(L, audio_playback_pause(child(L, PLAYBACK))); }
static int playback_drain(lua_State *L) { return success(L, audio_playback_drain(child(L, PLAYBACK))); }
static int capture_start(lua_State *L) { return success(L, audio_capture_start(child(L, CAPTURE))); }
static int capture_stop(lua_State *L) { return success(L, audio_capture_stop(child(L, CAPTURE))); }
static int mixer_sync(lua_State *L) { return success(L, audio_mixer_sync(child(L, MIXER))); }

static int playback_volume(lua_State *L)
{
    return success(L, audio_playback_set_volume(child(L, PLAYBACK), volume(L, 2)));
}

static int capture_volume(lua_State *L)
{
    return success(L, audio_capture_set_volume(child(L, CAPTURE), volume(L, 2)));
}

static int playback_ready(lua_State *L)
{
    struct audio_playback *p = child(L, PLAYBACK);
    lua_pushinteger(L, (lua_Integer)audio_playback_ready(p) * audio_playback_quantum(p));
    return 1;
}

static int capture_available(lua_State *L)
{
    lua_pushinteger(L, audio_capture_available(child(L, CAPTURE)));
    return 1;
}

static void integer_field(lua_State *L, const char *key, lua_Integer n)
{
    lua_pushinteger(L, n);
    lua_setfield(L, -2, key);
}

static void state_field(lua_State *L, int state)
{
    lua_pushstring(L, state == AUDIO_PLAYBACK_PAUSED ? "paused" :
                      state == AUDIO_PLAYBACK_RUNNING ? "running" : "error");
    lua_setfield(L, -2, "state");
}

static int playback_info(lua_State *L)
{
    struct audio_playback *p = child(L, PLAYBACK);
    lua_createtable(L, 0, 6);
    integer_field(L, "rate", audio_playback_rate(p));
    integer_field(L, "channels", audio_playback_channels(p));
    integer_field(L, "quantum", audio_playback_quantum(p));
    integer_field(L, "xruns", audio_playback_xruns(p));
    integer_field(L, "error", audio_playback_error(p));
    state_field(L, audio_playback_state(p));
    return 1;
}

static int capture_info(lua_State *L)
{
    struct audio_capture *p = child(L, CAPTURE);
    lua_createtable(L, 0, 6);
    integer_field(L, "rate", audio_capture_rate(p));
    integer_field(L, "channels", audio_capture_channels(p));
    integer_field(L, "quantum", audio_capture_quantum(p));
    integer_field(L, "xruns", audio_capture_xruns(p));
    integer_field(L, "error", audio_capture_error(p));
    state_field(L, audio_capture_state(p));
    return 1;
}

/* Return snapshots, never pointers into the dispatcher's mutable list. */
static int mixer_streams(lua_State *L)
{
    struct audio_mixer *m = child(L, MIXER);
    int count = audio_mixer_count(m);
    lua_createtable(L, count, 0);
    for (int i = 0; i < count; i++) {
        const struct audio_mixer_stream *s = audio_mixer_stream(m, i);
        lua_createtable(L, 0, 6);
        integer_field(L, "id", s->id);
        lua_pushstring(L, s->name);
        lua_setfield(L, -2, "name");
        lua_pushstring(L, s->direction ? "capture" : "playback");
        lua_setfield(L, -2, "direction");
        integer_field(L, "volume", s->volume);
        integer_field(L, "peak", s->peak);
        state_field(L, s->state);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int mixer_master(lua_State *L)
{
    struct audio_mixer *m = child(L, MIXER);
    if (lua_gettop(L) >= 2)
        return success(L, audio_mixer_set_master(m, volume(L, 2)));
    lua_pushinteger(L, audio_mixer_master(m));
    return 1;
}

static int mixer_volume(lua_State *L)
{
    struct audio_mixer *m = child(L, MIXER);
    lua_Integer id = luaL_checkinteger(L, 2);
    luaL_argcheck(L, id > 0 && (lua_Unsigned)id <= UINT32_MAX, 2, "invalid stream id");
    return success(L, audio_mixer_set_volume(m, (uint32_t)id, volume(L, 3)));
}

static int mixer_generation(lua_State *L)
{
    lua_pushinteger(L, audio_mixer_generation(child(L, MIXER)));
    return 1;
}

static const luaL_Reg connection_methods[] = {
    { "playback", connection_playback }, { "capture", connection_capture },
    { "mixer", connection_mixer }, { "fd", connection_fd },
    { "dispatch", connection_dispatch }, { "sync", connection_sync }, { NULL, NULL }
};
static const luaL_Reg playback_methods[] = {
    { "write", playback_write }, { "start", playback_start }, { "pause", playback_pause },
    { "drain", playback_drain }, { "volume", playback_volume }, { "ready", playback_ready },
    { "info", playback_info }, { NULL, NULL }
};
static const luaL_Reg capture_methods[] = {
    { "read", capture_read }, { "start", capture_start }, { "stop", capture_stop },
    { "volume", capture_volume }, { "available", capture_available },
    { "info", capture_info }, { NULL, NULL }
};
static const luaL_Reg mixer_methods[] = {
    { "streams", mixer_streams }, { "master", mixer_master }, { "volume", mixer_volume },
    { "generation", mixer_generation }, { "sync", mixer_sync }, { NULL, NULL }
};

static void metatable(lua_State *L, const char *name, const luaL_Reg *methods, lua_CFunction close)
{
    luaL_newmetatable(L, name);
    luaL_setfuncs(L, methods, 0);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, close);
    lua_setfield(L, -2, "close");
    lua_pushcfunction(L, close);
    lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, close);
    lua_setfield(L, -2, "__close");
    lua_pop(L, 1);
}

int luaopen_audio(lua_State *L)
{
    metatable(L, CONNECTION, connection_methods, connection_close);
    metatable(L, PLAYBACK, playback_methods, playback_close);
    metatable(L, CAPTURE, capture_methods, capture_close);
    metatable(L, MIXER, mixer_methods, mixer_close);
    lua_newtable(L);
    lua_pushcfunction(L, connect_audio);
    lua_setfield(L, -2, "connect");
    integer_field(L, "rate", 48000);
    integer_field(L, "channels", 2);
    integer_field(L, "frame_bytes", FRAME_BYTES);
    lua_pushliteral(L, "s16le");
    lua_setfield(L, -2, "format");
    return 1;
}
