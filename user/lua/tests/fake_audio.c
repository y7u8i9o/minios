/* Deterministic libaudio backend for the Lua ownership/conversion tests.
 * Real protocol, mixing and device I/O are exercised by lua_audio in QEMU. */
#include <audio/audio.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include "lua.h"
#include "lauxlib.h"

struct item {
    struct item *next;
    struct audio_connection *owner;
    int kind;
    struct audio_mixer_stream state;
    struct audio_mixer_stream view[32];
    int count;
};
struct audio_playback { struct item i; };
struct audio_capture { struct item i; };
struct audio_mixer { struct item i; };
struct audio_connection { struct item *items; unsigned master, generation; int16_t pcm[2]; int fd[2]; };
static int connections, live[3];
static uint32_t next_id;
static char fail_op[32];
static int fail_errno;
static ssize_t partial = -1;

static int failing(const char *op)
{
    if (strcmp(op, fail_op)) return 0;
    fail_op[0] = 0;
    errno = fail_errno;
    return 1;
}
static void *create(struct audio_connection *c, int kind, const char *name)
{
    if (failing("create")) return NULL;
    struct item *i = calloc(1, sizeof *i);
    assert(i);
    i->owner = c; i->kind = kind;
    i->state.id = __atomic_add_fetch(&next_id, 1, __ATOMIC_RELAXED);
    i->state.direction = kind == 1;
    i->state.volume = 100;
    strncpy(i->state.name, name, sizeof i->state.name - 1);
    i->next = c->items; c->items = i;
    __atomic_add_fetch(&live[kind], 1, __ATOMIC_RELAXED); c->generation++;
    return i;
}
static void destroy(struct item *i)
{
    if (!i) return;
    struct item **p = &i->owner->items;
    while (*p && *p != i) p = &(*p)->next;
    assert(*p == i);
    *p = i->next;
    __atomic_sub_fetch(&live[i->kind], 1, __ATOMIC_RELAXED); i->owner->generation++;
    free(i);
}
struct audio_connection *audio_connect(void)
{
    if (failing("connect")) return NULL;
    struct audio_connection *c = calloc(1, sizeof *c);
    assert(c); c->master = 100;
    assert(pipe(c->fd) == 0);
    __atomic_add_fetch(&connections, 1, __ATOMIC_RELAXED);
    return c;
}
void audio_disconnect(struct audio_connection *c)
{
    if (!c) return;
    while (c->items) destroy(c->items);
    close(c->fd[0]); close(c->fd[1]);
    __atomic_sub_fetch(&connections, 1, __ATOMIC_RELAXED); free(c);
}
int audio_connection_fd(const struct audio_connection *c) { assert(c); return c->fd[0]; }
int audio_connection_dispatch(struct audio_connection *c, int timeout)
{ assert(c && timeout >= -1); return failing("dispatch") ? -1 : 0; }
int audio_connection_sync(struct audio_connection *c)
{ assert(c); return failing("sync") ? -1 : 0; }
struct audio_playback *audio_playback_create(struct audio_connection *c, const char *name)
{ return create(c, 0, name); }
struct audio_capture *audio_capture_create(struct audio_connection *c, const char *name, int source)
{ assert(source == AUDIO_SOURCE_INPUT || source == AUDIO_SOURCE_MONITOR); return create(c, 1, name); }
struct audio_mixer *audio_mixer_create(struct audio_connection *c) { return create(c, 2, "mixer"); }
void audio_playback_destroy(struct audio_playback *p) { destroy((struct item *)p); }
void audio_capture_destroy(struct audio_capture *p) { destroy((struct item *)p); }
void audio_mixer_destroy(struct audio_mixer *p) { destroy((struct item *)p); }
static int active(struct item *i, int state)
{
    if (failing("control")) return -1;
    i->state.state = state; i->owner->generation++; return 0;
}
int audio_playback_start(struct audio_playback *p) { return active(&p->i, 1); }
int audio_playback_pause(struct audio_playback *p) { return active(&p->i, 0); }
int audio_playback_drain(struct audio_playback *p) { return active(&p->i, 0); }
int audio_capture_start(struct audio_capture *p) { return active(&p->i, 1); }
int audio_capture_stop(struct audio_capture *p) { return active(&p->i, 0); }
static int set_volume(struct item *i, unsigned percent)
{
    assert(percent <= 200);
    if (failing("control")) return -1;
    i->state.volume = percent; i->owner->generation++; return 0;
}
int audio_playback_set_volume(struct audio_playback *p, unsigned n) { return set_volume(&p->i, n); }
int audio_capture_set_volume(struct audio_capture *p, unsigned n) { return set_volume(&p->i, n); }
static size_t io_count(size_t n)
{
    if (partial >= 0 && (size_t)partial < n) n = (size_t)partial;
    partial = -1; return n;
}
ssize_t audio_playback_write(struct audio_playback *p, const int16_t *samples, size_t frames)
{
    assert(p);
    if (failing("write")) return -1;
    frames = io_count(frames);
    if (frames) memcpy(p->i.owner->pcm, samples, 4);
    return (ssize_t)frames;
}
ssize_t audio_capture_read(struct audio_capture *p, int16_t *samples, size_t frames)
{
    assert(p);
    if (failing("read")) return -1;
    frames = io_count(frames);
    for (size_t i = 0; i < frames; i++) memcpy(samples + i * 2, p->i.owner->pcm, 4);
    return (ssize_t)frames;
}
#define GETTERS(type) \
uint32_t audio_##type##_rate(const struct audio_##type *p) { assert(p); return 48000; } \
uint32_t audio_##type##_channels(const struct audio_##type *p) { assert(p); return 2; } \
uint32_t audio_##type##_quantum(const struct audio_##type *p) { assert(p); return 480; } \
uint32_t audio_##type##_xruns(const struct audio_##type *p) { assert(p); return 0; } \
int audio_##type##_state(const struct audio_##type *p) { return p->i.state.state; } \
int audio_##type##_error(const struct audio_##type *p) { assert(p); return 0; }
GETTERS(playback)
GETTERS(capture)
uint32_t audio_playback_ready(const struct audio_playback *p) { assert(p); return 3; }
uint32_t audio_capture_available(const struct audio_capture *p) { assert(p); return 480; }
int audio_mixer_sync(struct audio_mixer *m) { return audio_connection_sync(m->i.owner); }
int audio_mixer_count(const struct audio_mixer *m)
{
    struct item *i = (struct item *)&m->i;
    i->count = 0;
    for (struct item *s = i->owner->items; s; s = s->next)
        if (s->kind != 2 && i->count < 32) i->view[i->count++] = s->state;
    return i->count;
}
const struct audio_mixer_stream *audio_mixer_stream(const struct audio_mixer *m, int index)
{ assert(index >= 0 && index < m->i.count); return &m->i.view[index]; }
const struct audio_mixer_stream *audio_mixer_find(const struct audio_mixer *m, uint32_t id)
{
    for (int j = 0; j < audio_mixer_count(m); j++)
        if (m->i.view[j].id == id) return &m->i.view[j];
    return NULL;
}
unsigned audio_mixer_master(const struct audio_mixer *m) { return m->i.owner->master; }
uint32_t audio_mixer_generation(const struct audio_mixer *m) { return m->i.owner->generation; }
int audio_mixer_set_master(struct audio_mixer *m, unsigned n)
{
    assert(n <= 200);
    if (failing("control")) return -1;
    m->i.owner->master = n; m->i.owner->generation++; return 0;
}
int audio_mixer_set_volume(struct audio_mixer *m, uint32_t id, unsigned n)
{
    for (struct item *s = m->i.owner->items; s; s = s->next)
        if (s->state.id == id) return set_volume(s, n);
    errno = ENOENT; return -1;
}
static int stats(lua_State *L)
{
    lua_pushinteger(L, __atomic_load_n(&connections, __ATOMIC_RELAXED));
    for (int i = 0; i < 3; i++) lua_pushinteger(L, __atomic_load_n(&live[i], __ATOMIC_RELAXED));
    return 4;
}
static int fail(lua_State *L)
{
    const char *op = luaL_checkstring(L, 1);
    strncpy(fail_op, op, sizeof fail_op - 1);
    fail_errno = (int)luaL_checkinteger(L, 2);
    return 0;
}
static int partial_io(lua_State *L) { partial = luaL_checkinteger(L, 1); return 0; }
void audio_test_register(lua_State *L)
{
    static const luaL_Reg methods[] = { { "stats", stats }, { "fail", fail },
        { "partial", partial_io }, { NULL, NULL } };
    luaL_newlib(L, methods);
    lua_setglobal(L, "audio_test");
}
