#pragma once

/* Dynamic loading over the libraries that the loader mapped at start.
 * dlopen returns a handle for a library that is mapped in the process
 * (by its soname or its /lib path) and fails for any other; there is no
 * loading at run time. dlsym searches the dynamic symbol table of one
 * library, or of every mapped library for RTLD_DEFAULT. */
#define RTLD_LAZY 1
#define RTLD_NOW 2
#define RTLD_GLOBAL 0x100
#define RTLD_LOCAL 0
#define RTLD_DEFAULT ((void *)0)

void *dlopen(const char *file, int flags);
void *dlsym(void *handle, const char *name);
int dlclose(void *handle);
char *dlerror(void);
