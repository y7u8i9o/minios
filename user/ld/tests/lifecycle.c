/* Installed twice: dynamic with twenty dependency DSOs, and static. The
 * parent captures the complete event sequence through a pipe. */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>

#ifndef STATIC_PROBE
extern int dyn_value19(void);
extern char dyn_absolute_zero[];
extern void dyn_absent_weak(void) __attribute__((weak));
#endif

void dyn_record(char event)
{
    if (write(1, &event, 1) != 1)
        _exit(92);
}

static void before_libraries(void)
{
    if (!getenv("PATH") || fileno(stdout) != 1)
        _exit(93);
    dyn_record('p');
}
__attribute__((section(".preinit_array"), used))
static void (*const preinit)(void) = before_libraries;

__attribute__((constructor(200))) static void program_init_early(void) { dyn_record('u'); }
__attribute__((constructor(300))) static void program_init_late(void) { dyn_record('v'); }
__attribute__((destructor(200))) static void program_fini_early(void) { dyn_record('U'); }
__attribute__((destructor(300))) static void program_fini_late(void) { dyn_record('V'); }
static void on_exit(void) { dyn_record('x'); }

/* Thread local storage of the program itself, in both builds. */
static __thread int tls_probe = 11;
static __thread char tls_zero[24];

int main(int argc, char **argv)
{
    if (tls_probe != 11 || tls_zero[0] != 0)
        return 96;
    tls_probe++;
#ifndef STATIC_PROBE
    const void *volatile absolute_zero = dyn_absolute_zero;
    if (dyn_value19() != 20 || absolute_zero != NULL || dyn_absent_weak != NULL)
        return 94;
#endif
    dyn_record('m');
    if (argc > 1 && strcmp(argv[1], "quick") == 0)
        _exit(0);
    if (atexit(on_exit) != 0)
        return 95;
    return 0;
}
