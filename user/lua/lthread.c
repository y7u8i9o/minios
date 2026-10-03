/* Native workers with isolated Lua states. Only copied byte strings cross
 * states. GUI objects and Lua pointers never leave their owning thread.
 * The parent handle and worker each contain a reference to the job. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#ifdef MINIOS_HOST
#include <poll.h>
#else
#include <sys/ipc.h>
#endif
#include "lauxlib.h"
#include "lualib.h"
#include "minios.h"

#define THREAD_META "minios.thread"
#define MESSAGE_MAX 8192
#define QUEUE_BYTES 65536
#define QUEUE_MESSAGES 128
struct message { struct message *next; size_t size; char data[]; };
struct channel {
    struct message *head, *tail;
    size_t bytes;
    unsigned count;
    int fd[2], notified;
};
/* lock protects both channels and stop/done/ok. refs and active_workers are
 * atomic. Startup strings are immutable, error is published with completion.
 * No Lua allocation or callback occurs while lock is locked. */
struct job {
    pthread_mutex_t lock;
    unsigned refs;
    int stop, done, ok;
    struct channel input, output;
    char *path, *initial, *package_path;
    size_t initial_size;
    char error[1024];
};
struct handle { struct job *job; pthread_t tid; int joinable; };
static unsigned active_workers;

static struct job *current_job(lua_State *L)
{
    lua_getfield(L, LUA_REGISTRYINDEX, MINIOS_WORKER_KEY);
    struct job *j = lua_touserdata(L, -1);
    lua_pop(L, 1);
    return j;
}

static int failure(lua_State *L, int err)
{
    errno = err;
    return minios_errresult(L);
}
static void free_channel(struct channel *c)
{
    while (c->head) {
        struct message *next = c->head->next;
        free(c->head); c->head = next;
    }
    if (c->fd[0] >= 0) close(c->fd[0]);
    if (c->fd[1] >= 0) close(c->fd[1]);
}
static void unref(struct job *j)
{
    if (__atomic_sub_fetch(&j->refs, 1, __ATOMIC_ACQ_REL)) return;
    free_channel(&j->input); free_channel(&j->output);
    pthread_mutex_destroy(&j->lock);
    free(j->path); free(j->initial); free(j->package_path); free(j);
}
static int channel_init(struct channel *c)
{
    if (pipe(c->fd) < 0) return -1;
    for (int i = 0; i < 2; i++)
        if (fcntl(c->fd[i], F_SETFL, O_NONBLOCK) < 0 ||
            fcntl(c->fd[i], F_SETFD, FD_CLOEXEC) < 0) return -1;
    return 0;
}
/* One level-triggered byte means data, completion or cancellation. All
 * transitions are protected by job.lock, including consuming the byte. */
static void notify(struct job *j, struct channel *c)
{
    int ready = c->head || j->stop || j->done;
    char byte = 1;
    if (ready && !c->notified) {
        ssize_t n;
        do n = write(c->fd[1], &byte, 1); while (n < 0 && errno == EINTR);
        c->notified = 1;
    } else if (!ready && c->notified) {
        ssize_t n;
        do n = read(c->fd[0], &byte, 1); while (n < 0 && errno == EINTR);
        c->notified = 0;
    }
}
static void stop_job(struct job *j)
{
    pthread_mutex_lock(&j->lock);
    __atomic_store_n(&j->stop, 1, __ATOMIC_RELEASE);
    notify(j, &j->input); notify(j, &j->output);
    pthread_mutex_unlock(&j->lock);
}
static struct handle *check_handle(lua_State *L)
{
    struct handle *h = luaL_checkudata(L, 1, THREAD_META);
    luaL_argcheck(L, h->job != NULL, 1, "closed worker");
    return h;
}
static struct job *check_worker(lua_State *L)
{
    struct job *j = current_job(L);
    if (!j) luaL_error(L, "this operation requires a worker thread");
    return j;
}
static int send_message(lua_State *L, struct job *j, struct channel *c, int arg)
{
    size_t size;
    const char *data = luaL_checklstring(L, arg, &size);
    luaL_argcheck(L, size <= MESSAGE_MAX, arg, "message exceeds 8192 bytes");
    struct message *m = malloc(sizeof *m + size);
    if (!m) return failure(L, ENOMEM);
    m->next = NULL; m->size = size; memcpy(m->data, data, size);
    pthread_mutex_lock(&j->lock);
    int err = j->done || (j->stop && c == &j->input) ? EPIPE :
        c->count == QUEUE_MESSAGES || c->bytes + size > QUEUE_BYTES ? EAGAIN : 0;
    if (!err) {
        if (c->tail) c->tail->next = m; else c->head = m;
        c->tail = m; c->count++; c->bytes += size;
        notify(j, c);
    }
    pthread_mutex_unlock(&j->lock);
    if (err) { free(m); return failure(L, err); }
    lua_pushboolean(L, 1); return 1;
}
static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000 + t.tv_nsec / 1000000.0;
}
static int receive_message(lua_State *L, struct job *j, struct channel *c, int arg)
{
    lua_Integer timeout = luaL_optinteger(L, arg, 0);
    luaL_argcheck(L, timeout >= -1 && timeout <= 2147483647, arg, "invalid timeout");
    double deadline = timeout < 0 ? 0 : now_ms() + timeout;
    for (;;) {
        pthread_mutex_lock(&j->lock);
        struct message *m = c->head;
        int ended = j->done || j->stop;
        pthread_mutex_unlock(&j->lock);
        if (m) {
            /* A channel has exactly one receiving Lua state. Allocate before
             * removing its head, so a Lua allocation failure leaks no message
             * and cannot longjmp while a native mutex is locked. */
            luaL_Buffer b;
            char *data = luaL_buffinitsize(L, &b, m->size);
            size_t size = m->size;
            pthread_mutex_lock(&j->lock);
            c->head = m->next;
            if (!c->head) c->tail = NULL;
            c->count--; c->bytes -= size;
            memcpy(data, m->data, size);
            notify(j, c);
            pthread_mutex_unlock(&j->lock);
            free(m);
            luaL_pushresultsize(&b, size);
            return 1;
        }
        if (ended) return failure(L, EPIPE);
        double remaining = timeout < 0 ? -1 : deadline - now_ms();
        if (timeout >= 0 && remaining <= 0) return failure(L, ETIMEDOUT);
        int wait = remaining < 0 ? -1 : remaining >= 2147483647 ? 2147483647 : (int)remaining + 1;
        struct pollfd p = {c->fd[0], POLLIN, 0};
        int r = poll(&p, 1, wait);
        if (r < 0 && errno != EINTR) return minios_errresult(L);
    }
}
static int worker_exit(lua_State *L)
{
    return luaL_error(L, "os.exit is process-wide. Return from the worker script instead");
}
static int traceback(lua_State *L)
{
    const char *s = lua_tostring(L, 1);
    luaL_traceback(L, L, s ? s : "worker error", 1);
    return 1;
}
static int bootstrap(lua_State *L)
{
    struct job *j = lua_touserdata(L, 1);
    lua_pushlightuserdata(L, j); lua_setfield(L, LUA_REGISTRYINDEX, MINIOS_WORKER_KEY);
    luaL_openlibs(L);
    lua_getglobal(L, "package"); lua_pushstring(L, j->package_path);
    lua_setfield(L, -2, "path"); lua_pop(L, 1);
    lua_getglobal(L, "os"); lua_pushcfunction(L, worker_exit);
    lua_setfield(L, -2, "exit"); lua_pop(L, 1);
    lua_createtable(L, 1, 1);
    lua_pushstring(L, j->path); lua_rawseti(L, -2, 0);
    lua_pushlstring(L, j->initial, j->initial_size); lua_rawseti(L, -2, 1);
    lua_setglobal(L, "arg");
    if (luaL_loadfile(L, j->path) != LUA_OK) return lua_error(L);
    lua_pushlstring(L, j->initial, j->initial_size);
    lua_call(L, 1, 0);
    return 0;
}
static void *worker_main(void *arg)
{
    struct job *j = arg;
    lua_State *L = luaL_newstate();
    int status = LUA_ERRMEM;
    if (L) {
        lua_pushcfunction(L, traceback);
        lua_pushcfunction(L, bootstrap); lua_pushlightuserdata(L, j);
        status = lua_pcall(L, 1, 0, 1);
        if (status != LUA_OK) {
            const char *s = lua_tostring(L, -1);
            snprintf(j->error, sizeof j->error, "%s", s ? s : "worker failed");
        }
        /* Close all worker-owned audio/files before publishing completion. */
        lua_close(L);
    } else strcpy(j->error, "cannot allocate worker Lua state");
    pthread_mutex_lock(&j->lock);
    j->ok = status == LUA_OK; j->done = 1;
    notify(j, &j->input); notify(j, &j->output);
    pthread_mutex_unlock(&j->lock);
    __atomic_sub_fetch(&active_workers, 1, __ATOMIC_RELAXED);
    unref(j);
    return NULL;
}
static int spawn(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    size_t size;
    const char *initial = luaL_optlstring(L, 2, "", &size);
    luaL_argcheck(L, size <= MESSAGE_MAX, 2, "initial data exceeds 8192 bytes");
    lua_getglobal(L, "package"); lua_getfield(L, -1, "path");
    const char *search = luaL_checkstring(L, -1);
    struct handle *h = lua_newuserdatauv(L, sizeof *h, 0);
    memset(h, 0, sizeof *h); luaL_setmetatable(L, THREAD_META);
    struct job *j = calloc(1, sizeof *j);
    if (!j) return failure(L, ENOMEM);
    j->refs = 1;
    j->input.fd[0] = j->input.fd[1] = j->output.fd[0] = j->output.fd[1] = -1;
    int r = pthread_mutex_init(&j->lock, NULL);
    if (r) { free(j); return failure(L, r); }
    j->path = strdup(path); j->package_path = strdup(search);
    j->initial = malloc(size ? size : 1); j->initial_size = size;
    if (!j->path || !j->initial || !j->package_path) { unref(j); return failure(L, ENOMEM); }
    memcpy(j->initial, initial, size);
    if (channel_init(&j->input) < 0 || channel_init(&j->output) < 0) {
        int e = errno; unref(j); return failure(L, e);
    }
    j->refs = 2;
    __atomic_add_fetch(&active_workers, 1, __ATOMIC_RELAXED);
    r = pthread_create(&h->tid, NULL, worker_main, j);
    if (r) {
        __atomic_sub_fetch(&active_workers, 1, __ATOMIC_RELAXED);
        j->refs = 1; unref(j); return failure(L, r);
    }
    h->job = j; h->joinable = 1;
    return 1;
}
static int h_send(lua_State *L) { struct job *j = check_handle(L)->job; return send_message(L, j, &j->input, 2); }
static int h_receive(lua_State *L) { struct job *j = check_handle(L)->job; return receive_message(L, j, &j->output, 2); }
static int w_send(lua_State *L) { struct job *j = check_worker(L); return send_message(L, j, &j->output, 1); }
static int w_receive(lua_State *L) { struct job *j = check_worker(L); return receive_message(L, j, &j->input, 1); }
static int h_fd(lua_State *L) { lua_pushinteger(L, check_handle(L)->job->output.fd[0]); return 1; }
static int w_fd(lua_State *L) { lua_pushinteger(L, check_worker(L)->input.fd[0]); return 1; }
static int h_stop(lua_State *L) { stop_job(check_handle(L)->job); lua_pushboolean(L, 1); return 1; }
static int stopped(lua_State *L) { lua_pushboolean(L, __atomic_load_n(&check_worker(L)->stop, __ATOMIC_ACQUIRE)); return 1; }
static int active(lua_State *L) { lua_pushinteger(L, __atomic_load_n(&active_workers, __ATOMIC_RELAXED)); return 1; }
static int h_status(lua_State *L)
{
    struct job *j = check_handle(L)->job;
    pthread_mutex_lock(&j->lock);
    int done = j->done, ok = j->ok;
    pthread_mutex_unlock(&j->lock);
    lua_pushstring(L, !done ? "running" : ok ? "done" : "error");
    return 1;
}
static int h_join(lua_State *L)
{
    struct handle *h = check_handle(L);
    if (h->joinable) {
        int r = pthread_join(h->tid, NULL);
        if (r) return failure(L, r);
        h->joinable = 0;
    }
    if (!h->job->ok) { lua_pushnil(L); lua_pushstring(L, h->job->error); return 2; }
    lua_pushboolean(L, 1); return 1;
}
static int h_close(lua_State *L)
{
    struct handle *h = luaL_checkudata(L, 1, THREAD_META);
    if (!h->job) { lua_pushboolean(L, 1); return 1; }
    stop_job(h->job);
    int n = h_join(L);
    if (h->joinable) return n;
    unref(h->job); h->job = NULL;
    return n;
}
static int h_gc(lua_State *L)
{
    struct handle *h = luaL_checkudata(L, 1, THREAD_META);
    if (h->job) {
        stop_job(h->job);
        if (h->joinable) pthread_detach(h->tid);
        unref(h->job); h->job = NULL; h->joinable = 0;
    }
    return 0;
}
int luaopen_thread(lua_State *L)
{
    static const luaL_Reg methods[] = {
        {"send", h_send}, {"receive", h_receive}, {"fd", h_fd}, {"stop", h_stop},
        {"status", h_status}, {"join", h_join}, {"close", h_close}, {NULL, NULL}
    };
    static const luaL_Reg funcs[] = {
        {"spawn", spawn}, {"send", w_send}, {"receive", w_receive}, {"fd", w_fd},
        {"stop_requested", stopped}, {"active", active}, {NULL, NULL}
    };
    luaL_newmetatable(L, THREAD_META);
    lua_newtable(L); luaL_setfuncs(L, methods, 0); lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, h_gc); lua_setfield(L, -2, "__gc");
    lua_pushcfunction(L, h_close); lua_setfield(L, -2, "__close");
    lua_pop(L, 1);
    luaL_newlib(L, funcs);
    return 1;
}
