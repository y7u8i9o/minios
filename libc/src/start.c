#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <minios/syscall.h>
#include <errno.h>

int errno;
char **environ;

int main(int argc, char **argv, char **envp);
void __stdio_init(void);
void __stdio_flush_all(void);

long __syscall_ret(long r)
{
    if (r < 0 && r > -4096) {
        errno = (int)-r;
        return -1;
    }
    return r;
}

#define ATEXIT_MAX 16
static void (*atexit_fns[ATEXIT_MAX])(void);
static int atexit_count;

int atexit(void (*fn)(void))
{
    if (atexit_count == ATEXIT_MAX)
        return -1;
    atexit_fns[atexit_count++] = fn;
    return 0;
}

void exit(int status)
{
    while (atexit_count > 0)
        atexit_fns[--atexit_count]();
    __stdio_flush_all();
    _exit(status);
}

void _Exit(int status)
{
    _exit(status);
}

void abort(void)
{
    static const char msg[] = "abort()\n";
    write(2, msg, sizeof msg - 1);
    _exit(134);
}

void __assert_fail(const char *expr, const char *file, int line, const char *func)
{
    fprintf(stderr, "%s:%d: %s: assertion `%s' failed\n", file, line, func, expr);
    abort();
}

__attribute__((noreturn)) void __libc_start(int argc, char **argv, char **envp)
{
    environ = envp;
    __stdio_init();
    exit(main(argc, argv, envp));
}
