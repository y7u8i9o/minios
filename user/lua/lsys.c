/* The sys module: starting programs, opening files by type, signals
 * and the system identification. Failures return nil, the message and
 * the errno, like io.open. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <sched.h>
#include <gui/mime.h>
#include "lauxlib.h"
#include "minios.h"

#ifdef MINIOS_HOST
/* The host unit test compiles this file with the system libc, which
 * lacks the minios time and processor calls. */
#include <time.h>
static int sleep_ms(unsigned long ms)
{
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000 };
    return nanosleep(&ts, NULL);
}
static long uptime_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
static int nproc(void) { return 1; }
static int getcpu(void) { return 0; }
#endif

/* Collects the string arguments from index first into a NULL terminated
 * vector on the C heap. */
static char **argv_from(lua_State *L, int first)
{
    int top = lua_gettop(L);
    int count = top >= first ? top - first + 1 : 0;
    char **argv = malloc(((size_t)count + 1) * sizeof *argv);
    if (!argv)
        luaL_error(L, "out of memory");
    for (int i = 0; i < count; i++)
        argv[i] = (char *)luaL_checkstring(L, first + i);
    argv[count] = NULL;
    return argv;
}

/* sys.spawn(program, args...): starts the program as a child of init,
 * for programs that outlive the caller such as launched applications. */
static int sys_spawn(lua_State *L)
{
    luaL_checkstring(L, 1);
    char **argv = argv_from(L, 1);
    int r = mime_spawn(argv);
    free(argv);
    if (r < 0) {
        errno = -r;
        return minios_errresult(L);
    }
    lua_pushboolean(L, 1);
    return 1;
}

/* sys.run(program, args...): runs the program to completion and
 * returns true or nil, then "exit" or "signal", then the number. */
static int sys_run(lua_State *L)
{
    const char *prog = luaL_checkstring(L, 1);
    char **argv = argv_from(L, 1);
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        execvp(prog, argv);
        _exit(127);
    }
    free(argv);
    if (pid < 0)
        return minios_errresult(L);
    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return minios_errresult(L);
    }
    const char *what = "exit";
    int code;
    if (WIFEXITED(status)) {
        code = WEXITSTATUS(status);
    } else {
        what = "signal";
        code = WTERMSIG(status);
    }
    if (strcmp(what, "exit") == 0 && code == 0)
        lua_pushboolean(L, 1);
    else
        lua_pushnil(L);
    lua_pushstring(L, what);
    lua_pushinteger(L, code);
    return 3;
}

/* sys.open(path): starts the program registered for the file's type,
 * or the exec line of a launcher. */
static int sys_open(lua_State *L)
{
    int r = (int)mime_open(luaL_checkstring(L, 1));
    if (r < 0) {
        errno = -r;
        return minios_errresult(L);
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int sys_type(lua_State *L)
{
    const char *path = luaL_checkstring(L, 1);
    struct stat st;
    int is_dir = stat(path, &st) == 0 && S_ISDIR(st.st_mode);
    lua_pushstring(L, mime_type(path, is_dir));
    return 1;
}

/* sys.mime_load(types, apps): reads the tables from other paths; the
 * host unit test uses it, programs on the target rely on the defaults. */
static int sys_mime_load(lua_State *L)
{
    int r = mime_load(luaL_checkstring(L, 1), luaL_checkstring(L, 2));
    if (r < 0) {
        errno = -r;
        return minios_errresult(L);
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int sys_handler(lua_State *L)
{
    const char *prog = mime_handler(luaL_checkstring(L, 1));
    if (prog)
        lua_pushstring(L, prog);
    else
        lua_pushnil(L);
    return 1;
}

static int sys_pid(lua_State *L)
{
    lua_pushinteger(L, getpid());
    return 1;
}

static int sys_ppid(lua_State *L)
{
    lua_pushinteger(L, getppid());
    return 1;
}

static const struct { const char *name; int sig; } signals[] = {
    { "HUP", SIGHUP }, { "INT", SIGINT }, { "QUIT", SIGQUIT }, { "KILL", SIGKILL },
    { "TERM", SIGTERM }, { "USR1", SIGUSR1 }, { "USR2", SIGUSR2 }, { "CHLD", SIGCHLD },
    { "STOP", SIGSTOP }, { "CONT", SIGCONT },
};

static int sys_kill(lua_State *L)
{
    pid_t pid = (pid_t)luaL_checkinteger(L, 1);
    int sig = SIGTERM;
    if (lua_type(L, 2) == LUA_TSTRING) {
        const char *name = lua_tostring(L, 2);
        sig = -1;
        for (size_t i = 0; i < sizeof signals / sizeof signals[0]; i++)
            if (strcmp(signals[i].name, name) == 0)
                sig = signals[i].sig;
        if (sig < 0)
            return luaL_argerror(L, 2, "unknown signal name");
    } else if (!lua_isnoneornil(L, 2)) {
        sig = (int)luaL_checkinteger(L, 2);
    }
    if (kill(pid, sig) < 0)
        return minios_errresult(L);
    lua_pushboolean(L, 1);
    return 1;
}

static int sys_sleep(lua_State *L)
{
    sleep_ms((unsigned long)luaL_checkinteger(L, 1));
    return 0;
}

static int sys_uptime(lua_State *L)
{
    lua_pushinteger(L, uptime_ms());
    return 1;
}

static int sys_uname(lua_State *L)
{
    struct utsname u;
    if (uname(&u) < 0)
        return minios_errresult(L);
    lua_createtable(L, 0, 5);
    lua_pushstring(L, u.sysname);
    lua_setfield(L, -2, "sysname");
    lua_pushstring(L, u.nodename);
    lua_setfield(L, -2, "nodename");
    lua_pushstring(L, u.release);
    lua_setfield(L, -2, "release");
    lua_pushstring(L, u.version);
    lua_setfield(L, -2, "version");
    lua_pushstring(L, u.machine);
    lua_setfield(L, -2, "machine");
    return 1;
}

static int sys_nproc(lua_State *L)
{
    lua_pushinteger(L, nproc());
    return 1;
}

static int sys_cpu(lua_State *L)
{
    lua_pushinteger(L, getcpu());
    return 1;
}

static int sys_yield(lua_State *L)
{
    sched_yield();
    return 0;
}

static const luaL_Reg sys_funcs[] = {
    { "spawn", sys_spawn },
    { "run", sys_run },
    { "open", sys_open },
    { "type", sys_type },
    { "handler", sys_handler },
    { "mime_load", sys_mime_load },
    { "pid", sys_pid },
    { "ppid", sys_ppid },
    { "kill", sys_kill },
    { "sleep", sys_sleep },
    { "uptime", sys_uptime },
    { "uname", sys_uname },
    { "nproc", sys_nproc },
    { "cpu", sys_cpu },
    { "yield", sys_yield },
    { NULL, NULL }
};

int luaopen_sys(lua_State *L)
{
    luaL_newlib(L, sys_funcs);
    return 1;
}
