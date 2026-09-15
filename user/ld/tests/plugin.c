/* libldplugin.so: loaded by dlopen only. Its events are appended to a
 * buffer of the program (found through the program's exported symbol),
 * so that construction and destruction order is observable after the
 * library is gone. It has thread local storage, which every thread of
 * the process gets on first use, needs libldplugdep.so and calls back
 * into libldtls.so, which is loaded at start. */
#include <string.h>

extern char dltest_events[64];             /* exported by the program */
extern long plugdep_double(long v);
extern int *tls_counter_address(void);

__thread int plugin_tls = 30;
static int plugin_calls;

void plugin_event(char event)
{
    size_t n = strlen(dltest_events);
    if (n + 1 < sizeof dltest_events) {
        dltest_events[n] = event;
        dltest_events[n + 1] = '\0';
    }
}

__attribute__((constructor)) static void plugin_init(void) { plugin_event('p'); }
__attribute__((destructor)) static void plugin_fini(void) { plugin_event('P'); }

int plugin_count(void)
{
    return ++plugin_calls;
}

long plugin_compute(long v)
{
    return plugdep_double(v) + *tls_counter_address();
}

int *plugin_tls_address(void)
{
    return &plugin_tls;
}
