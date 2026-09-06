/* Each build of this file supplies one node of a twenty-library chain.
 * The event stream proves dependency order independently of link order. */
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>

#define JOIN_(a, b) a##b
#define JOIN(a, b) JOIN_(a, b)
extern void dyn_record(char event);
#if LEVEL > 0
extern int JOIN(dyn_value, PREVIOUS)(void);
#endif
static int ready;

__attribute__((constructor)) static void chain_init(void)
{
    /* Constructors must run after libc has installed the main thread's
     * errno storage, allocator and streams. Check all three here. */
    char *p = malloc(32);
    errno = 0;
    if (!p || close(999) != -1 || errno != EBADF || fileno(stdout) != 1)
        _exit(91);
    snprintf(p, 32, "%d", LEVEL);
    free(p);
    ready = 1;
    dyn_record((char)('a' + LEVEL));
}

__attribute__((destructor)) static void chain_fini(void)
{
    dyn_record((char)('A' + LEVEL));
}

int JOIN(dyn_value, LEVEL)(void)
{
#if LEVEL > 0
    return ready + JOIN(dyn_value, PREVIOUS)();
#else
    return ready;
#endif
}

#if LEVEL == 0
/* Exercise the older DT_INIT/DT_FINI hooks together with array entries. */
void dyn_legacy_init(void) { dyn_record('!'); }
void dyn_legacy_fini(void) { dyn_record('?'); }
#endif
