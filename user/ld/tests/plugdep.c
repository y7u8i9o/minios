/* libldplugdep.so: needed only by the plugin, so that dlopen loads it as
 * part of the plugin's group and dlclose unloads it with the plugin. */
extern void plugin_event(char event);

__thread long plugdep_value = 21;

__attribute__((constructor)) static void plugdep_init(void) { plugin_event('d'); }
__attribute__((destructor)) static void plugdep_fini(void) { plugin_event('D'); }

long plugdep_double(long v)
{
    plugdep_value += v;
    return 2 * v;
}
