/* Descriptor and clock operations used by workers and event loops. Raw fds
 * belong to the caller; borrowed audio/thread fds are only for polling. */
#if defined(MINIOS_HOST) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <sys/resource.h>
#ifdef MINIOS_HOST
#include <poll.h>
#ifdef __APPLE__
#include <pthread.h>
#include <mach/mach.h>
#else
#include <sys/syscall.h>
#endif
#else
#include <sys/ipc.h>
#endif
#include "lauxlib.h"
#include "minios.h"

static int fdarg(lua_State *L, int arg)
{
    lua_Integer n = luaL_checkinteger(L, arg);
    luaL_argcheck(L, n >= 0 && n <= INT_MAX, arg, "invalid descriptor");
    return (int)n;
}
static int ok(lua_State *L, int r)
{
    if (r < 0) return minios_errresult(L);
    lua_pushboolean(L, 1); return 1;
}
static int open_fd(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    int flags = (int)luaL_optinteger(L, 2, O_RDONLY);
    int mode = (int)luaL_optinteger(L, 3, 0666);
    int fd = open(path, flags | O_CLOEXEC, mode);
    if (fd < 0) return minios_errresult(L);
    lua_pushinteger(L, fd); return 1;
}
static int nonblock(lua_State *L)
{
    int fd = fdarg(L, 1), flags = fcntl(fd, F_GETFL);
    if (flags < 0) return minios_errresult(L);
    if (lua_isnone(L, 2)) { lua_pushboolean(L, (flags & O_NONBLOCK) != 0); return 1; }
    return ok(L, fcntl(fd, F_SETFL, lua_toboolean(L, 2) ? flags | O_NONBLOCK : flags & ~O_NONBLOCK));
}
static int make_pipe(lua_State *L)
{
    int fd[2];
    if (pipe(fd) < 0) return minios_errresult(L);
    for (int i = 0; i < 2; i++) {
        if (fcntl(fd[i], F_SETFD, FD_CLOEXEC) < 0 ||
            (lua_toboolean(L, 1) && fcntl(fd[i], F_SETFL, O_NONBLOCK) < 0)) {
            int e = errno; close(fd[0]); close(fd[1]); errno = e;
            return minios_errresult(L);
        }
    }
    lua_pushinteger(L, fd[0]); lua_pushinteger(L, fd[1]); return 2;
}
static int write_fd(lua_State *L)
{
    int fd = fdarg(L, 1);
    size_t size;
    const char *s = luaL_checklstring(L, 2, &size);
    ssize_t n = write(fd, s, size);
    if (n < 0) return minios_errresult(L);
    lua_pushinteger(L, n); return 1;
}
static int poll_fds(lua_State *L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_Integer timeout = luaL_optinteger(L, 2, -1);
    luaL_argcheck(L, timeout >= -1 && timeout <= INT_MAX, 2, "invalid timeout");
    size_t n = lua_rawlen(L, 1);
    luaL_argcheck(L, n <= 1024, 1, "too many descriptors");
    struct pollfd *p = lua_newuserdatauv(L, n * sizeof *p, 0);
    for (size_t i = 0; i < n; i++) {
        lua_rawgeti(L, 1, (lua_Integer)i + 1); luaL_checktype(L, -1, LUA_TTABLE);
        lua_getfield(L, -1, "fd"); p[i].fd = fdarg(L, -1); lua_pop(L, 1);
        lua_getfield(L, -1, "events");
        lua_Integer events = luaL_optinteger(L, -1, POLLIN);
        luaL_argcheck(L, events >= 0 && (events & ~(POLLIN | POLLOUT)) == 0, 1, "invalid poll events");
        p[i].events = (short)events; p[i].revents = 0;
        lua_pop(L, 2);
    }
    int r = poll(p, (unsigned)n, (int)timeout);
    if (r < 0) return minios_errresult(L);
    for (size_t i = 0; i < n; i++) {
        lua_rawgeti(L, 1, (lua_Integer)i + 1);
        lua_pushinteger(L, p[i].revents); lua_setfield(L, -2, "revents"); lua_pop(L, 1);
    }
    lua_pushinteger(L, r); return 1;
}
static int clock_ns(lua_State *L)
{
    static const char *const names[] = {"monotonic", "realtime", NULL};
    int which = luaL_checkoption(L, 1, "monotonic", names);
    struct timespec t;
    if (clock_gettime(which ? CLOCK_REALTIME : CLOCK_MONOTONIC, &t) < 0) return minios_errresult(L);
    lua_pushinteger(L, (lua_Integer)t.tv_sec * 1000000000 + t.tv_nsec); return 1;
}
static void field(lua_State *L, const char *key, lua_Integer value)
{
    lua_pushinteger(L, value); lua_setfield(L, -2, key);
}
static int usage(lua_State *L)
{
    static const char *const names[] = {"process", "thread", "children", NULL};
    int which = luaL_checkoption(L, 1, "process", names);
#ifdef MINIOS_HOST
#ifdef __APPLE__
    if (which == 1) {
        thread_basic_info_data_t info;
        mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
        mach_port_t self = mach_thread_self();
        kern_return_t r = thread_info(self, THREAD_BASIC_INFO, (thread_info_t)&info, &count);
        mach_port_deallocate(mach_task_self(), self);
        if (r != KERN_SUCCESS) { errno = EIO; return minios_errresult(L); }
        lua_Integer u = (lua_Integer)info.user_time.seconds * 1000000000 + info.user_time.microseconds * 1000;
        lua_Integer s = (lua_Integer)info.system_time.seconds * 1000000000 + info.system_time.microseconds * 1000;
        lua_createtable(L, 0, 3); field(L, "user_ns", u); field(L, "system_ns", s); field(L, "cpu_ns", u + s);
        return 1;
    }
#endif
#endif
    int who = which == 2 ? RUSAGE_CHILDREN : RUSAGE_SELF;
#ifdef RUSAGE_THREAD
    if (which == 1) who = RUSAGE_THREAD;
#endif
    struct rusage ru;
    if (getrusage(who, &ru) < 0) return minios_errresult(L);
    lua_Integer u = (lua_Integer)ru.ru_utime.tv_sec * 1000000000 + ru.ru_utime.tv_usec * 1000;
    lua_Integer s = (lua_Integer)ru.ru_stime.tv_sec * 1000000000 + ru.ru_stime.tv_usec * 1000;
    lua_createtable(L, 0, 6); field(L, "user_ns", u); field(L, "system_ns", s); field(L, "cpu_ns", u + s);
    field(L, "voluntary_switches", ru.ru_nvcsw); field(L, "involuntary_switches", ru.ru_nivcsw);
#ifdef __APPLE__
    field(L, "max_rss_kb", ru.ru_maxrss / 1024);
#else
    field(L, "max_rss_kb", ru.ru_maxrss);
#endif
    return 1;
}
static int thread_id(lua_State *L)
{
#ifdef MINIOS_HOST
#ifdef __APPLE__
    uint64_t id;
    int r = pthread_threadid_np(NULL, &id);
    if (r) { errno = r; return minios_errresult(L); }
#else
    long id = syscall(SYS_gettid);
#endif
#else
    int id = gettid();
#endif
    lua_pushinteger(L, (lua_Integer)id); return 1;
}
void minios_sys_extra(lua_State *L)
{
    static const luaL_Reg funcs[] = {
        {"open_fd", open_fd}, {"nonblock", nonblock}, {"pipe", make_pipe},
        {"write", write_fd}, {"poll", poll_fds}, {"clock_ns", clock_ns},
        {"usage", usage}, {"thread_id", thread_id}, {NULL, NULL}
    };
    luaL_setfuncs(L, funcs, 0);
#define CONST(name) field(L, #name, name)
    CONST(POLLIN); CONST(POLLOUT); CONST(POLLERR); CONST(POLLHUP); CONST(POLLNVAL);
    CONST(O_RDONLY); CONST(O_WRONLY); CONST(O_RDWR); CONST(O_CREAT); CONST(O_TRUNC);
    CONST(O_APPEND); CONST(O_EXCL); CONST(O_NONBLOCK);
    lua_newtable(L);
    CONST(EAGAIN); CONST(EINTR); CONST(ETIMEDOUT); CONST(EPIPE); CONST(EINVAL);
    lua_setfield(L, -2, "errno");
#undef CONST
}
