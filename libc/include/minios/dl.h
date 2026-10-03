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
 * The thread pointer addresses a thread control block (struct dl_tcb)
 * whose first word points to the thread's struct pthread and whose second
 * word is the dynamic thread vector. The TLS blocks of the objects loaded
 * at start lie at fixed offsets from the thread pointer, and the blocks of
 * objects loaded with dlopen are allocated on first use through the
 * vector. On x86_64 (TLS variant II, the FS base) the control block is the
 * struct pthread itself and the blocks lie below it; on aarch64 (variant
 * I, TPIDR_EL0) the control block is 16 bytes after the struct pthread and
 * the blocks lie above it. dl_tls_block and dl_tls_place below state both
 * layouts. */
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
    void *self;                 /* the struct pthread of the thread */
    struct dl_dtv *dtv;
};

#if defined(__aarch64__)
#define DL_TLS_ABOVE_TP 1       /* variant I */
#define DL_TLS_TCB_SIZE 16      /* the control block, before the first block */
#else
#define DL_TLS_ABOVE_TP 0       /* variant II */
#define DL_TLS_TCB_SIZE 0
#endif

/* The start of the static block at offset from the thread pointer tp. */
static inline unsigned char *dl_tls_block(void *tp, size_t offset)
{
#if DL_TLS_ABOVE_TP
    return (unsigned char *)tp + offset;
#else
    return (unsigned char *)tp - offset;
#endif
}

/* The offset from the thread pointer to a symbol at symbol_offset in the
 * static block at offset: the value of an initial-exec relocation. */
static inline uint64_t dl_tls_tprel(uint64_t symbol_offset, size_t offset)
{
#if DL_TLS_ABOVE_TP
    return symbol_offset + offset;
#else
    return symbol_offset - offset;
#endif
}

/* Add a block of memsz bytes aligned to align to the static area, whose
 * size so far is *total (DL_TLS_TCB_SIZE when empty); returns the block's
 * offset. The program's block is placed first, at the offset the linker
 * assumed for its local-exec accesses. */
static inline size_t dl_tls_place(size_t *total, size_t memsz, size_t align)
{
#if DL_TLS_ABOVE_TP
    size_t offset = (*total + align - 1) & ~(align - 1);
    *total = offset + memsz;
    return offset;
#else
    *total = (*total + memsz + align - 1) & ~(align - 1);
    return *total;
#endif
}

struct dl_dtv_entry {
    void *block;                /* aligned start of the module's block */
    void *raw;                  /* the allocation containing it */
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
    /* Bytes of the static TLS area beside a thread control block (below it
     * in variant II, above it including the control block in variant I) and
     * the alignment the thread pointer needs; the C library reserves that
     * space for each thread. */
    size_t static_tls_size, static_tls_align;
    /* Copy the static TLS images beside the control block at the thread
     * pointer tcb and clear its vector. */
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
