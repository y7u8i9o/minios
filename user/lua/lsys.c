/* The sys module: starting programs, signals and the system
 * identification. Failures return nil, the message and the errno, like
 * io.open. The MIME types and handlers are in the mime module (lmime.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <sched.h>
#include <minios/conf.h>
#include "lauxlib.h"
#include "minios.h"

#ifdef MINIOS_HOST
/* The host unit test compiles this file with the system libc, which
 * lacks the minios sleep and processor calls. uptime_ms comes from the
 * fake client of libgui, which the test links. */
#include <time.h>
static int sleep_ms(unsigned long ms)
{
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000 };
    return nanosleep(&ts, NULL);
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
 * for programs that outlive the caller such as launched applications. An
 * intermediate child starts it and exits at once, which hands the program
 * to init. The program follows the language of the desktop settings. */
static int sys_spawn(lua_State *L)
{
    luaL_checkstring(L, 1);
    char **argv = argv_from(L, 1);
    pid_t pid = fork();
    if (pid == 0) {
        pid_t grandchild = fork();
        if (grandchild == 0) {
            conf_export_locale();
            execvp(argv[0], argv);
            _exit(127);
        }
        _exit(grandchild < 0 ? 1 : 0);
    }
    free(argv);
    if (pid < 0)
        return minios_errresult(L);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        errno = EAGAIN;
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

/* sys.spawn_pipe(program, args...): starts the program with its standard
 * output and error joined into a pipe and its input from /dev/null.
 * Returns the pid and the read end of the pipe, to be watched with
 * app:watch and read with sys.read. */
static int sys_spawn_pipe(lua_State *L)
{
    const char *prog = luaL_checkstring(L, 1);
    char **argv = argv_from(L, 1);
    int fds[2];
    if (pipe(fds) < 0) {
        free(argv);
        return minios_errresult(L);
    }
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        int null = open("/dev/null", O_RDONLY);
        if (null >= 0) {
            dup2(null, 0);
            close(null);
        }
        dup2(fds[1], 1);
        dup2(fds[1], 2);
        close(fds[0]);
        close(fds[1]);
        execvp(prog, argv);
        _exit(127);
    }
    free(argv);
    close(fds[1]);
    if (pid < 0) {
        close(fds[0]);
        return minios_errresult(L);
    }
    lua_pushinteger(L, pid);
    lua_pushinteger(L, fds[0]);
    return 2;
}

/* sys.read(fd [, max]): the bytes available, "" when nothing is there
 * yet, nil at the end of the stream. */
static int sys_read(lua_State *L)
{
    int fd = (int)luaL_checkinteger(L, 1);
    size_t max = (size_t)luaL_optinteger(L, 2, 4096);
    luaL_Buffer b;
    char *p = luaL_buffinitsize(L, &b, max);
    ssize_t n = read(fd, p, max);
    if (n < 0) {
        if (errno == EAGAIN || errno == EINTR) {
            luaL_pushresultsize(&b, 0);
            return 1;
        }
        return minios_errresult(L);
    }
    if (n == 0) {
        lua_pushnil(L);
        return 1;
    }
    luaL_pushresultsize(&b, (size_t)n);
    return 1;
}

static int sys_close(lua_State *L)
{
    if (close((int)luaL_checkinteger(L, 1)) < 0)
        return minios_errresult(L);
    lua_pushboolean(L, 1);
    return 1;
}

/* sys.wait(pid [, nohang]): "exit" or "signal" and the number once the
 * process ended; nil while it runs when nohang is true. */
static int sys_wait(lua_State *L)
{
    pid_t pid = (pid_t)luaL_checkinteger(L, 1);
    int flags = lua_toboolean(L, 2) ? WNOHANG : 0;
    int status;
    pid_t r;
    while ((r = waitpid(pid, &status, flags)) < 0 && errno == EINTR)
        ;
    if (r < 0)
        return minios_errresult(L);
    if (r == 0) {
        lua_pushnil(L);
        return 1;
    }
    if (WIFEXITED(status)) {
        lua_pushstring(L, "exit");
        lua_pushinteger(L, WEXITSTATUS(status));
    } else {
        lua_pushstring(L, "signal");
        lua_pushinteger(L, WTERMSIG(status));
    }
    return 2;
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
    lua_Integer ms = luaL_checkinteger(L, 1);
    luaL_argcheck(L, ms >= 0 && ms <= 2147483647, 1, "invalid sleep duration");
    if (sleep_ms((unsigned long)ms) < 0) return minios_errresult(L);
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
    { "spawn_pipe", sys_spawn_pipe },
    { "read", sys_read },
    { "close", sys_close },
    { "wait", sys_wait },
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
    minios_sys_extra(L);
    return 1;
}
