#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <minios/syscall.h>
#include <errno.h>
#include "thread/tcb.h"
#include <minios/dl.h>

char **environ;
void __pthread_init_main(void);

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
static struct __libc_lock atexit_lock = __LIBC_LOCK_INIT;

int atexit(void (*fn)(void))
{
    __libc_lock_lock(&atexit_lock);
    int r = -1;
    if (atexit_count < ATEXIT_MAX) {
        atexit_fns[atexit_count++] = fn;
        r = 0;
    }
    __libc_lock_unlock(&atexit_lock);
    return r;
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

/* Static executables use the linker's array boundaries directly. Dynamic
 * programs delegate the entire dependency graph, including their own
 * arrays, to the loader. Weak references keep a program without arrays
 * valid and avoid imposing initialization sections on the static init. */
extern void (*__preinit_array_start[])(void) __attribute__((weak));
extern void (*__preinit_array_end[])(void) __attribute__((weak));
extern void (*__init_array_start[])(void) __attribute__((weak));
extern void (*__init_array_end[])(void) __attribute__((weak));
extern void (*__fini_array_start[])(void) __attribute__((weak));
extern void (*__fini_array_end[])(void) __attribute__((weak));
extern void _init(void) __attribute__((weak));
extern void _fini(void) __attribute__((weak));

static void static_fini(void)
{
    if (__fini_array_start && __fini_array_end) {
        for (void (**fn)(void) = __fini_array_end; fn != __fini_array_start; )
            (*--fn)();
    }
    if (_fini)
        _fini();
}

__attribute__((noreturn)) void __libc_start(int argc, char **argv, char **envp,
                                          void (*initialize)(int, char **, char **),
                                          void (*finalize)(void))
{
    /* sysret uses rcx for the entry address when exec enters a static
     * program. Only an interpreter can supply callback registers: consult
     * AT_BASE in the kernel's auxiliary vector before trusting either. */
    const uintptr_t *aux = (const uintptr_t *)envp;
    while (*aux)
        aux++;
    aux++;
    int has_loader = 0;
    for (const uintptr_t *a = aux; a[0] != 0; a += 2) {
        if (a[0] == 7)             /* AT_BASE */
            has_loader = a[1] != 0;
    }
    if (!has_loader) {
        initialize = NULL;
        finalize = NULL;
    }
    /* Thread local storage comes before the control block that anchors
     * it; the loader's allocator entries wait for a usable heap. */
    __tls_init(aux);
    __pthread_init_main();
    if (__dl_interface) {
        __dl_interface->alloc = malloc;
        __dl_interface->free = free;
    }
    environ = envp;
    if (argc > 0 && argv[0] != NULL)
        setprogname(argv[0]);
    __stdio_init();
    /* Register teardown before calling constructors: their atexit handlers
     * must run while their libraries still exist. Initialization follows
     * pthread and stdio setup so constructors can use errno and libc. */
    atexit(finalize ? finalize : static_fini);
    if (initialize) {
        initialize(argc, argv, envp);
    } else {
        if (__preinit_array_start && __preinit_array_end) {
            for (void (**fn)(void) = __preinit_array_start; fn != __preinit_array_end; fn++)
                (*fn)();
        }
        if (_init)
            _init();
        if (__init_array_start && __init_array_end) {
            for (void (**fn)(void) = __init_array_start; fn != __init_array_end; fn++)
                (*fn)();
        }
    }
    exit(main(argc, argv, envp));
}
