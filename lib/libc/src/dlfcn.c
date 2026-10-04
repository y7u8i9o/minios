/* dlopen and friends forward to the loader through its interface record
 * (minios/dl.h); the loader contains the objects, their scopes and the
 * error text. Without a loader every call fails with one message. */
#include <dlfcn.h>
#include <stddef.h>

static const char *static_error;

void *dlopen(const char *file, int mode)
{
    if (!__dl_interface) {
        static_error = "dlopen: no dynamic loader in a static program";
        return NULL;
    }
    return __dl_interface->open(file, mode);
}

void *dlsym(void *handle, const char *name)
{
    if (!__dl_interface) {
        static_error = "dlsym: no dynamic loader in a static program";
        return NULL;
    }
    return __dl_interface->sym(handle, name);
}

int dlclose(void *handle)
{
    if (!__dl_interface) {
        static_error = "dlclose: no dynamic loader in a static program";
        return -1;
    }
    return __dl_interface->close(handle);
}

char *dlerror(void)
{
    if (!__dl_interface) {
        const char *text = static_error;
        static_error = NULL;
        return (char *)text;
    }
    return (char *)__dl_interface->error();
}
