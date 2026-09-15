#pragma once
/* The interface between /lib/ld.so and the C library, and the thread
 * local storage layout both agree on (docs/design/dynlink.md).
 *
 * The loader finds the variable __dl_interface in the C library after
 * relocating the initial objects and stores the address of its interface
 * record there; a static program leaves it NULL. The C library fills in
 * the allocator entries before it creates a second thread or calls
 * dlopen.
 *
 * Thread local storage follows the x86-64 variant II layout: the thread
 * pointer (the FS base) addresses the thread control block, whose first
 * word is the block's own address and whose second word is the dynamic
 * thread vector; the TLS blocks of the objects loaded at start lie below
 * the control block at fixed offsets, and the blocks of objects loaded
 * with dlopen are allocated on first use through the vector. */
#include <stddef.h>
#include <stdint.h>

#define DL_INTERFACE_VERSION 1

/* One object with a PT_TLS segment. The loader writes the address of
 * this record, not a small integer, as the module id of DTPMOD64
 * relocations, so that __tls_get_addr reaches the record without a
 * table. A static module (loaded at start) has a nonzero offset and its
 * block at tp - offset; a dynamic module (loaded by dlopen) has offset
 * zero and occupies slot index of every thread's vector. */
struct dl_tls_module {
    uintptr_t image;            /* the initialization image, filesz bytes */
    size_t filesz, memsz, align;
    size_t offset;
    size_t index;
    unsigned generation;        /* distinguishes reuses of the same slot */
};

/* The argument of __tls_get_addr: module is a struct dl_tls_module *. */
struct dl_tls_index {
    uintptr_t module;
    size_t offset;
};

/* The first two words of a thread control block. */
struct dl_tcb {
    void *self;
    struct dl_dtv *dtv;
};

struct dl_dtv_entry {
    void *block;                /* aligned start of the module's block */
    void *raw;                  /* the allocation holding it */
    unsigned generation;
};

struct dl_dtv {
    size_t count;
    struct dl_dtv_entry entries[];
};

/* dlopen modes; dlfcn.h repeats them for programs. */
#define DL_LAZY 1
#define DL_NOW 2
#define DL_GLOBAL 0x100
#define DL_LOCAL 0

struct dl_interface {
    unsigned version;
    /* Bytes of static TLS below a thread control block and the alignment
     * the block needs; the C library reserves that space for each thread. */
    size_t static_tls_size, static_tls_align;
    /* Copy the static TLS images below a control block and clear its vector. */
    void (*tls_setup)(void *tcb);
    /* The address of offset inside the calling thread's block of a module. */
    void *(*tls_get_addr)(struct dl_tls_module *module, size_t offset);
    /* Release the dynamic blocks and the vector of an exiting thread. */
    void (*tls_free)(void *tcb);
    void *(*open)(const char *name, int mode);
    void *(*sym)(void *handle, const char *name);
    int (*close)(void *handle);
    /* The text of the last failure of open, sym or close, then cleared. */
    const char *(*error)(void);
    /* Filled in by the C library. */
    void *(*alloc)(size_t size);
    void (*free)(void *p);
};

extern struct dl_interface *__dl_interface;
