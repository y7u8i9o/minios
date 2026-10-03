#pragma once
#include <minios/dl.h>

/* Dynamic loading through /lib/ld.so (docs/design/dynlink.md). dlopen
 * maps a shared object by its soname from /lib and the package prefix,
 * or by path when the name contains a slash, together with the libraries
 * it needs, and returns a handle; a name that is loaded already returns
 * its object. dlopen(NULL) is the program, whose handle searches every
 * object in the global scope, as RTLD_DEFAULT does. dlsym searches the
 * handle's object and the objects it was loaded with. dlclose drops a
 * reference and unloads what nothing refers to any more. dlerror gives
 * the text of the last failure once. A static program has no loader,
 * so dlopen fails there. */
#define RTLD_LAZY DL_LAZY
#define RTLD_NOW DL_NOW
#define RTLD_GLOBAL DL_GLOBAL
#define RTLD_LOCAL DL_LOCAL
#define RTLD_DEFAULT ((void *)0)

void *dlopen(const char *file, int mode);
void *dlsym(void *handle, const char *name);
int dlclose(void *handle);
char *dlerror(void);
