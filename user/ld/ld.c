/* MiniOS ELF64 dynamic loader, /lib/ld.so.
 *
 * Startup has three distinct phases: relocate this freestanding loader,
 * map and relocate the program's dependency graph, then let libc initialize
 * its thread state and standard streams before calling ELF initializers.
 * start.S passes the initialization and finalization callbacks to crt0.
 *
 * After startup the loader stays in the process for three services that
 * libc reaches through the record described in minios/dl.h: thread local
 * storage (the static layout of the initial objects and the blocks of
 * objects loaded later), dlopen with its symbol lookup and unloading, and
 * lazy binding of the procedure linkage table entries of objects linked
 * without -z now, which _dl_runtime_resolve in start.S hands to _dl_fixup.
 *
 * Metadata lives in a small mmap arena so that object addresses stay
 * stable as dependencies are discovered; no libc allocation or TLS is
 * used here, and the loader has its own recursive lock (dl_lock) that
 * every runtime entry takes. Errors during startup print a diagnostic
 * and exit; the same checks during dlopen and dlsym return to the caller
 * through dl_setjmp with the diagnostic as the dlerror text.
 */
#include <stdint.h>
#include <stddef.h>
#include <syscall_nums.h>
#include <minios/abi.h>
#include <minios/dl.h>
#include <ld_arch.h>

#define PAGE 4096UL
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
/* The system libraries, the libraries of packages (docs/design/packages.md),
 * then the software that the package installer does not manage
 * (minios/local.h). The libraries of the user in ~/.local/lib come last,
 * and not at all for a program that runs with changed ids, which the
 * kernel marks with AT_SECURE (docs/design/users.md). */
static const char *const lib_dirs[] = { "/lib/", "/usr/lib/", "/usr/local/lib/" };
static int secure;
static char *home_lib;              /* "$HOME/.local/lib/" or NULL */
/* Keep DSOs above the interpreter and below the ordinary mmap area. Every
 * image reserves its holes too, so later mappings cannot occupy them. */
#define LIB_ARENA 0x7e0010000000UL
#define LIB_LIMIT 0x7e8000000000UL
#define LIB_ALIGN 0x100000UL

struct ehdr {
    uint8_t e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};
struct phdr {
    uint32_t p_type, p_flags;
    uint64_t p_offset, p_vaddr, p_paddr, p_filesz, p_memsz, p_align;
};
struct dyn { int64_t d_tag; uint64_t d_val; };
struct sym {
    uint32_t st_name;
    uint8_t st_info, st_other;
    uint16_t st_shndx;
    uint64_t st_value, st_size;
};
struct rela { uint64_t r_offset, r_info; int64_t r_addend; };

#define ET_DYN 3
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_TLS 7
#define PT_GNU_RELRO 0x6474e552
#define PF_X 1
#define PF_W 2
#define PF_R 4
#define DT_NULL 0
#define DT_NEEDED 1
#define DT_PLTRELSZ 2
#define DT_PLTGOT 3
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_RELAENT 9
#define DT_STRSZ 10
#define DT_SYMENT 11
#define DT_INIT 12
#define DT_FINI 13
#define DT_REL 17
#define DT_RELSZ 18
#define DT_PLTREL 20
#define DT_TEXTREL 22
#define DT_JMPREL 23
#define DT_BIND_NOW 24
#define DT_INIT_ARRAY 25
#define DT_FINI_ARRAY 26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28
#define DT_FLAGS 30
#define DT_PREINIT_ARRAY 32
#define DT_PREINIT_ARRAYSZ 33
#define DT_RELRSZ 35
#define DT_RELR 36
#define DT_GNU_HASH 0x6ffffef5
#define DT_VERDEFNUM 0x6ffffffd
#define DT_VERNEEDNUM 0x6fffffff
#define DF_BIND_NOW 8
#define STB_GLOBAL 1
#define STB_WEAK 2
#define STT_TLS 6
#define STT_GNU_IFUNC 10
#define STV_INTERNAL 1
#define STV_HIDDEN 2
#define STV_PROTECTED 3
#define SHN_UNDEF 0
#define SHN_ABS 0xfff1
#define AT_NULL 0
#define AT_PHDR 3
#define AT_PHENT 4
#define AT_PHNUM 5
#define AT_BASE 7
#define AT_ENTRY 9
#define AT_SECURE 23

struct object;

/* The objects a symbol lookup walks, in order. The global scope holds
 * the initial objects and those opened with RTLD_GLOBAL; every dlopen
 * builds a local scope of the opened object and its dependencies. */
struct scope {
    struct object **objects;
    size_t count, capacity;
};

/* All addresses in these records are relocated process addresses. Program
 * headers remain available for checking every table, symbol and write. */
struct object {
    const char *name;
    uintptr_t base;
    uintptr_t map_start;            /* the reserved span, for unloading */
    size_t map_size;
    const struct phdr *phdr;
    size_t phnum;
    const struct dyn *dyn;
    size_t ndyn;
    const struct sym *symtab;
    size_t nsyms, gnu_nsyms;
    const char *strtab;
    size_t strsz;
    const uint32_t *hash, *gnu_hash;
    const struct rela *rela, *jmprel;
    size_t relasz, pltrelsz;
    uintptr_t *pltgot;              /* DT_PLTGOT, the table lazy binding fills */
    int bind_now;                   /* DF_BIND_NOW or DT_BIND_NOW */
    int lazy;                       /* JUMP_SLOT entries are resolved on first call */
    uintptr_t init, fini;
    const uintptr_t *preinit_array, *init_array, *fini_array;
    size_t preinit_size, init_size, fini_size;
    struct dl_tls_module *tls;      /* NULL without a PT_TLS segment */
    struct scope *local;            /* the scope its relocations search after the global one */
    int global;                     /* member of the global scope */
    int permanent;                  /* loaded at start, never unloaded */
    unsigned refs;                  /* dlopen references of its scope; protected by dl_lock */
    struct object *next;            /* the list of loaded objects; protected by dl_lock after startup */
    /* Iterative DFS state for dependency-first initialization. Objects in
     * a cycle are visited once. fini_next records the actual call order. */
    struct object *init_parent, *fini_next;
    size_t next_needed;
    unsigned init_state;
};

static struct object *objects, *last_object, *fini_head, *free_objects;
static struct object *loading;      /* the object load_library is building, for recovery */
static struct scope global_scope;
static uintptr_t next_base = LIB_ARENA;
static unsigned char *arena;
static size_t arena_left;
static int argc_saved;
static char **argv_saved, **envp_saved;
extern struct dyn _DYNAMIC[] __attribute__((visibility("hidden")));

/* Thread local storage: the dynamic modules occupy vector slots handed
 * out from this table (index is slot + 1); generations never repeat. */
static struct dl_tls_module **dynamic_modules;
static size_t dynamic_count, dynamic_capacity;
static unsigned tls_generation;

static struct dl_interface interface;

/* Failure reporting for the runtime entries: die stores the text here and
 * returns to the entry through the jump buffer while recovering is set. */
static char error_text[256];
static int error_set, recovering;
static uintptr_t recover[LD_ARCH_JMPBUF_WORDS];
int _dl_setjmp(uintptr_t *buffer) __attribute__((returns_twice));
void _dl_longjmp(uintptr_t *buffer, int value) __attribute__((noreturn));
void _dl_runtime_resolve(void);

static long sys(long nr, long a, long b, long c, long d, long e, long f)
{
    return ld_arch_syscall(nr, a, b, c, d, e, f);
}

static size_t dl_strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

static int dl_strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static void dl_memcpy(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--)
        *d++ = *s++;
}

static void dl_memset(void *dst, int c, size_t n)
{
    unsigned char *d = dst;
    while (n--)
        *d++ = (unsigned char)c;
}

static void put(const char *s)
{
    size_t left = dl_strlen(s);
    while (left) {
        long n = sys(SYS_write, 2, (long)s, (long)left, 0, 0, 0);
        if (n <= 0)
            return;
        s += n;
        left -= (size_t)n;
    }
}

static void append(char *buffer, size_t size, size_t *used, const char *s)
{
    while (*s && *used + 1 < size)
        buffer[(*used)++] = *s++;
    buffer[*used] = '\0';
}

__attribute__((noreturn)) static void die(const char *what, const char *name)
{
    if (recovering) {
        size_t used = 0;
        append(error_text, sizeof error_text, &used, what);
        if (name) {
            append(error_text, sizeof error_text, &used, ": ");
            append(error_text, sizeof error_text, &used, name);
        }
        error_set = 1;
        _dl_longjmp(recover, 1);
    }
    put("ld.so: ");
    put(what);
    if (name) {
        put(": ");
        put(name);
    }
    put("\n");
    sys(SYS_exit, 127, 0, 0, 0, 0, 0);
    for (;;)
        ;
}

/* ---- the loader lock ----
 * Recursive, since constructors run by dlopen may call dlopen or reach a
 * lazily bound entry. A contended thread sleeps on the state word. */
static struct { int state; int owner; int depth; } dl_lock_state;

static int current_tid(void)
{
    return (int)sys(SYS_gettid, 0, 0, 0, 0, 0, 0);
}

static void dl_lock(void)
{
    int tid = current_tid();
    if (dl_lock_state.depth && dl_lock_state.owner == tid) {
        dl_lock_state.depth++;
        return;
    }
    for (;;) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&dl_lock_state.state, &expected, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            break;
        sys(SYS_futex, (long)&dl_lock_state.state, FUTEX_WAIT, 1, 0, 0, 0);
    }
    dl_lock_state.owner = tid;
    dl_lock_state.depth = 1;
}

static void dl_unlock(void)
{
    if (--dl_lock_state.depth)
        return;
    dl_lock_state.owner = 0;
    __atomic_store_n(&dl_lock_state.state, 0, __ATOMIC_RELEASE);
    sys(SYS_futex, (long)&dl_lock_state.state, FUTEX_WAKE, 1, 0, 0, 0);
}

static struct dl_tcb *current_tcb(void)
{
    return ld_arch_thread_pointer();
}

/* Subtraction-based checks avoid accepting wrapped ELF offsets. */
static int within(uint64_t offset, uint64_t size, uint64_t limit)
{
    return offset <= limit && size <= limit - offset;
}

static int power_of_two(uint64_t value)
{
    return value && !(value & (value - 1));
}

static void *dl_alloc(size_t size)
{
    if (size > SIZE_MAX - PAGE)
        die("metadata size overflow", NULL);
    size = ALIGN_UP(size, 16UL);
    if (size > arena_left) {
        size_t bytes = ALIGN_UP(size, PAGE);
        if (bytes < 4 * PAGE)
            bytes = 4 * PAGE;
        long r = sys(SYS_mmap, 0, (long)bytes, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (r < 0)
            die("cannot allocate loader metadata", NULL);
        arena = (void *)r;
        arena_left = bytes;
    }
    void *p = arena;
    arena += size;
    arena_left -= size;
    dl_memset(p, 0, size);
    return p;
}

static struct object *new_object(const char *name)
{
    struct object *o = free_objects;
    if (o) {
        free_objects = o->next;
        dl_memset(o, 0, sizeof *o);
    } else {
        o = dl_alloc(sizeof *o);
    }
    o->name = name;
    return o;
}

/* Objects join the loaded list only when they are complete, so that a
 * lookup from another thread never sees a partially built record. */
static void commit_object(struct object *o)
{
    o->next = NULL;
    if (last_object)
        last_object->next = o;
    else
        objects = o;
    last_object = o;
}

static void remove_object(struct object *o)
{
    struct object **p = &objects;
    while (*p && *p != o)
        p = &(*p)->next;
    if (*p)
        *p = o->next;
    if (last_object == o) {
        last_object = objects;
        while (last_object && last_object->next)
            last_object = last_object->next;
    }
}

static void scope_add(struct scope *s, struct object *o)
{
    for (size_t i = 0; i < s->count; i++)
        if (s->objects[i] == o)
            return;
    if (s->count == s->capacity) {
        size_t capacity = s->capacity ? 2 * s->capacity : 8;
        struct object **grown = dl_alloc(capacity * sizeof *grown);
        for (size_t i = 0; i < s->count; i++)
            grown[i] = s->objects[i];
        s->objects = grown;
        s->capacity = capacity;
    }
    s->objects[s->count++] = o;
}

static void scope_remove(struct scope *s, const struct object *o)
{
    size_t j = 0;
    for (size_t i = 0; i < s->count; i++)
        if (s->objects[i] != o)
            s->objects[j++] = s->objects[i];
    s->count = j;
}

/* A range must be contained in one actual load segment, not merely in
 * the image's bounding box: gaps and page padding are not ELF tables. */
static int contains(const struct object *o, uintptr_t addr, size_t bytes, unsigned flags)
{
    if (addr < o->base)
        return 0;
    uint64_t offset = addr - o->base;
    for (size_t i = 0; i < o->phnum; i++) {
        const struct phdr *p = &o->phdr[i];
        if (p->p_type == PT_LOAD && (p->p_flags & flags) == flags && offset >= p->p_vaddr &&
            within(offset - p->p_vaddr, bytes, p->p_memsz))
            return 1;
    }
    return 0;
}

static uintptr_t address(const struct object *o, uint64_t value)
{
    if (value > UINTPTR_MAX - o->base)
        die("ELF address overflow", o->name);
    return o->base + value;
}

static void require_range(const struct object *o, uintptr_t addr, size_t bytes, unsigned flags)
{
    if (!contains(o, addr, bytes, flags))
        die("ELF range outside load segments", o->name);
}

static const char *string_at(const struct object *o, size_t offset)
{
    if (offset < o->strsz) {
        for (size_t i = offset; i < o->strsz; i++)
            if (!o->strtab[i])
                return o->strtab + offset;
    }
    die("invalid dynamic string", o->name);
}

/* SysV hashing carries an explicit symbol count. GNU hashing omits one:
 * the last nonempty bucket's terminating chain defines the highest symbol
 * index. Validate the chain while deriving that bound, before relocation
 * code can use an index supplied by the file. */
static void validate_hashes(struct object *o)
{
    size_t sysv_count = 0;
    if (o->hash) {
        require_range(o, (uintptr_t)o->hash, 2 * sizeof(uint32_t), PF_R);
        uint32_t buckets = o->hash[0], chains = o->hash[1];
        if (!buckets || !chains)
            die("empty SysV hash table", o->name);
        require_range(o, (uintptr_t)o->hash, (2UL + buckets + chains) * sizeof(uint32_t), PF_R);
        sysv_count = chains;
        const uint32_t *indices = o->hash + 2;
        for (size_t i = 0; i < (size_t)buckets + chains; i++)
            if (indices[i] >= chains)
                die("invalid SysV hash index", o->name);
        o->nsyms = chains;
    }
    if (o->gnu_hash) {
        const uint32_t *h = o->gnu_hash;
        require_range(o, (uintptr_t)h, 4 * sizeof *h, PF_R);
        if (!h[0] || !power_of_two(h[2]) || h[3] >= 32 || !h[1])
            die("invalid GNU hash header", o->name);
        size_t prefix = 4 * sizeof *h + (size_t)h[2] * sizeof(uint64_t) + (size_t)h[0] * sizeof *h;
        require_range(o, (uintptr_t)h, prefix, PF_R);
        const uint32_t *buckets = (const void *)((const uint64_t *)(h + 4) + h[2]);
        const uint32_t *chains = buckets + h[0];
        uint32_t highest = 0;
        for (uint32_t i = 0; i < h[0]; i++) {
            if (buckets[i] && buckets[i] < h[1])
                die("invalid GNU hash bucket", o->name);
            if (buckets[i] > highest)
                highest = buckets[i];
        }
        size_t count = h[1];
        if (highest) {
            size_t index = highest;
            for (;;) {
                uintptr_t slot = (uintptr_t)chains + (index - h[1]) * sizeof *chains;
                require_range(o, slot, sizeof *chains, PF_R);
                uint32_t chain = *(const uint32_t *)slot;
                if (++index > UINT32_MAX)
                    die("GNU hash index overflow", o->name);
                if (chain & 1) {
                    count = index;
                    break;
                }
            }
        }
        if (sysv_count && count > sysv_count)
            die("inconsistent hash symbol counts", o->name);
        require_range(o, (uintptr_t)chains, (count - h[1]) * sizeof *chains, PF_R);
        o->gnu_nsyms = count;
        if (!sysv_count)
            o->nsyms = count;
    }
    if (!o->nsyms)
        die("object without a supported hash table", o->name);
    require_range(o, (uintptr_t)o->symtab, o->nsyms * sizeof *o->symtab, PF_R);
}

/* The TLS segment of an object: its image must lie in a readable load
 * segment; its alignment is a power of two. The module's offset or slot
 * is assigned later, once it is known whether the object is initial. */
static void parse_tls(struct object *o, const struct phdr *p)
{
    if (o->tls)
        die("multiple TLS segments", o->name);
    if (p->p_filesz > p->p_memsz || (p->p_align > 1 && !power_of_two(p->p_align)))
        die("invalid TLS segment", o->name);
    if (!p->p_memsz)
        return;
    struct dl_tls_module *m = dl_alloc(sizeof *m);
    m->image = address(o, p->p_vaddr);
    m->filesz = p->p_filesz;
    m->memsz = p->p_memsz;
    m->align = p->p_align > 1 ? p->p_align : 1;
    if (m->filesz)
        require_range(o, m->image, m->filesz, PF_R);
    o->tls = m;
}

/* Only the formats which this loader understands may reach relocation.
 * Unsupported REL/RELR, version requirements and text relocations
 * receive diagnostics instead of being silently ignored. */
static void parse_dynamic(struct object *o)
{
    const struct phdr *dynamic = NULL;
    for (size_t i = 0; i < o->phnum; i++) {
        const struct phdr *p = &o->phdr[i];
        if (p->p_type == PT_TLS)
            parse_tls(o, p);
        if (p->p_type == PT_DYNAMIC) {
            if (dynamic)
                die("multiple dynamic segments", o->name);
            dynamic = p;
        }
    }
    if (!dynamic || !dynamic->p_memsz || dynamic->p_memsz % sizeof(struct dyn))
        die("invalid dynamic segment", o->name);
    o->dyn = (const void *)address(o, dynamic->p_vaddr);
    require_range(o, (uintptr_t)o->dyn, dynamic->p_memsz, PF_R);
    size_t limit = dynamic->p_memsz / sizeof(struct dyn);
    size_t syment = 0, relaent = 0, pltrel = 0;
    for (o->ndyn = 0; o->ndyn < limit; o->ndyn++) {
        const struct dyn *d = &o->dyn[o->ndyn];
        if (d->d_tag == DT_NULL)
            break;
        switch (d->d_tag) {
        case DT_HASH: o->hash = (const void *)address(o, d->d_val); break;
        case DT_GNU_HASH: o->gnu_hash = (const void *)address(o, d->d_val); break;
        case DT_STRTAB: o->strtab = (const void *)address(o, d->d_val); break;
        case DT_STRSZ: o->strsz = d->d_val; break;
        case DT_SYMTAB: o->symtab = (const void *)address(o, d->d_val); break;
        case DT_SYMENT: syment = d->d_val; break;
        case DT_RELA: o->rela = (const void *)address(o, d->d_val); break;
        case DT_RELASZ: o->relasz = d->d_val; break;
        case DT_RELAENT: relaent = d->d_val; break;
        case DT_JMPREL: o->jmprel = (const void *)address(o, d->d_val); break;
        case DT_PLTRELSZ: o->pltrelsz = d->d_val; break;
        case DT_PLTREL: pltrel = d->d_val; break;
        case DT_PLTGOT: o->pltgot = (void *)address(o, d->d_val); break;
        case DT_BIND_NOW: o->bind_now = 1; break;
        case DT_FLAGS: if (d->d_val & DF_BIND_NOW) o->bind_now = 1; break;
        case DT_INIT: o->init = d->d_val ? address(o, d->d_val) : 0; break;
        case DT_FINI: o->fini = d->d_val ? address(o, d->d_val) : 0; break;
        case DT_PREINIT_ARRAY: o->preinit_array = (const void *)address(o, d->d_val); break;
        case DT_PREINIT_ARRAYSZ: o->preinit_size = d->d_val; break;
        case DT_INIT_ARRAY: o->init_array = (const void *)address(o, d->d_val); break;
        case DT_INIT_ARRAYSZ: o->init_size = d->d_val; break;
        case DT_FINI_ARRAY: o->fini_array = (const void *)address(o, d->d_val); break;
        case DT_FINI_ARRAYSZ: o->fini_size = d->d_val; break;
        case DT_TEXTREL: die("text relocations are not supported", o->name);
        case DT_REL: case DT_RELSZ: case DT_RELR: case DT_RELRSZ:
            if (d->d_val)
                die("REL and RELR relocations are not supported", o->name);
            break;
        case DT_VERDEFNUM: case DT_VERNEEDNUM:
            if (d->d_val)
                die("symbol versioning is not supported", o->name);
            break;
        default: break;
        }
    }
    if (o->ndyn == limit)
        die("unterminated dynamic segment", o->name);
    if (!o->symtab || !o->strtab || !o->strsz || syment != sizeof(struct sym))
        die("invalid dynamic symbol table", o->name);
    require_range(o, (uintptr_t)o->strtab, o->strsz, PF_R);
    validate_hashes(o);
    if (o->relasz % sizeof(struct rela) || o->pltrelsz % sizeof(struct rela) ||
        (o->relasz && relaent != sizeof(struct rela)) || (o->pltrelsz && pltrel != DT_RELA))
        die("invalid relocation entry size or format", o->name);
    if (o->relasz)
        require_range(o, (uintptr_t)o->rela, o->relasz, PF_R);
    if (o->pltrelsz)
        require_range(o, (uintptr_t)o->jmprel, o->pltrelsz, PF_R);
    if (o->pltrelsz && o->pltgot)
        require_range(o, (uintptr_t)o->pltgot, 3 * sizeof(uintptr_t), PF_W);
    const uintptr_t *arrays[] = { o->preinit_array, o->init_array, o->fini_array };
    const size_t sizes[] = { o->preinit_size, o->init_size, o->fini_size };
    for (size_t i = 0; i < 3; i++) {
        if (sizes[i] % sizeof(uintptr_t))
            die("invalid initializer array size", o->name);
        if (sizes[i])
            require_range(o, (uintptr_t)arrays[i], sizes[i], PF_R);
    }
    if (o != objects && o->preinit_size)
        die("preinit array in a shared library", o->name);
    if (o->init)
        require_range(o, o->init, 1, PF_X);
    if (o->fini)
        require_range(o, o->fini, 1, PF_X);
}

static uint32_t elf_hash(const char *name)
{
    uint32_t h = 0;
    for (const unsigned char *p = (const void *)name; *p; p++) {
        h = (h << 4) + *p;
        uint32_t g = h & 0xf0000000u;
        h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

static uint32_t gnu_hash(const char *name)
{
    uint32_t h = 5381;
    for (const unsigned char *p = (const void *)name; *p; p++)
        h = h * 33 + *p;
    return h;
}

static const struct sym *symbol_match(const struct object *o, uint32_t index, const char *name)
{
    if (index >= o->nsyms)
        die("symbol index outside table", o->name);
    const struct sym *s = &o->symtab[index];
    unsigned bind = s->st_info >> 4, visibility = s->st_other & 3;
    if (s->st_shndx == SHN_UNDEF || (bind != STB_GLOBAL && bind != STB_WEAK) ||
        visibility == STV_HIDDEN || visibility == STV_INTERNAL)
        return NULL;
    return dl_strcmp(string_at(o, s->st_name), name) == 0 ? s : NULL;
}

static const struct sym *lookup_in(const struct object *o, const char *name)
{
    if (o->gnu_hash) {
        const uint32_t *h = o->gnu_hash;
        const uint64_t *bloom = (const void *)(h + 4);
        const uint32_t *buckets = (const void *)(bloom + h[2]);
        const uint32_t *chains = buckets + h[0];
        uint32_t hash = gnu_hash(name);
        uint64_t mask = (1UL << (hash % 64)) | (1UL << ((hash >> h[3]) % 64));
        if ((bloom[(hash / 64) & (h[2] - 1)] & mask) != mask)
            return NULL;
        uint32_t index = buckets[hash % h[0]];
        if (!index)
            return NULL;
        for (; index < o->gnu_nsyms; index++) {
            uint32_t chain = chains[index - h[1]];
            if ((chain | 1) == (hash | 1)) {
                const struct sym *s = symbol_match(o, index, name);
                if (s)
                    return s;
            }
            if (chain & 1)
                return NULL;
        }
        die("unterminated GNU hash chain", o->name);
    }
    const uint32_t *buckets = o->hash + 2;
    const uint32_t *chains = buckets + o->hash[0];
    uint32_t index = buckets[elf_hash(name) % o->hash[0]];
    for (size_t steps = 0; index; steps++) {
        if (steps >= o->nsyms)
            die("cyclic SysV hash chain", o->name);
        const struct sym *s = symbol_match(o, index, name);
        if (s)
            return s;
        index = chains[index];
    }
    return NULL;
}

struct definition { const struct object *object; const struct sym *symbol; };

/* A separate found/not-found result permits an absolute symbol whose
 * value is zero. The global scope precedes the local one; the program,
 * first in the global scope, is skipped for its own COPY relocations. */
static int lookup(const char *name, const struct object *skip, const struct scope *local,
                  struct definition *out)
{
    const struct scope *scopes[2] = { &global_scope, local };
    for (int k = 0; k < 2; k++) {
        const struct scope *s = scopes[k];
        if (!s || (k == 1 && s == &global_scope))
            continue;
        for (size_t i = 0; i < s->count; i++) {
            const struct object *o = s->objects[i];
            if (o == skip)
                continue;
            const struct sym *sym = lookup_in(o, name);
            if (sym) {
                out->object = o;
                out->symbol = sym;
                return 1;
            }
        }
    }
    return 0;
}

static uintptr_t symbol_address(const struct definition *def)
{
    const struct sym *s = def->symbol;
    unsigned type = s->st_info & 15;
    if (type == STT_TLS || type == STT_GNU_IFUNC)
        die("TLS and IFUNC symbols are not supported here", def->object->name);
    if (s->st_shndx == SHN_ABS)
        return s->st_value;
    uintptr_t value = address(def->object, s->st_value);
    require_range(def->object, value, s->st_size, 0);
    return value;
}

/* The definition a symbolic relocation of o refers to. A local or
 * protected definition binds within its own object; other definitions
 * interpose in scope order, including the executable's copy-relocation
 * destination. Returns 0 for an undefined weak symbol. */
static int resolve(const struct object *o, const struct rela *r, uint32_t type, struct definition *def)
{
    uint32_t index = (uint32_t)(r->r_info >> 32);
    if (index >= o->nsyms)
        die("relocation symbol index outside table", o->name);
    const struct sym *s = &o->symtab[index];
    def->object = o;
    def->symbol = s;
    unsigned visibility = s->st_other & 3;
    if (type != RELOC_COPY && s->st_shndx != SHN_UNDEF && ((s->st_info >> 4) == 0 || visibility != 0))
        return 1;
    if (index == 0 && (type == RELOC_TLS_DTPMOD || type == RELOC_TLS_DTPREL || type == RELOC_TLS_TPREL))
        return 1;                   /* a local TLS symbol: the object itself, the addend as the offset */
    const char *name = string_at(o, s->st_name);
    if (lookup(name, type == RELOC_COPY ? objects : NULL, o->local, def))
        return 1;
    if ((s->st_info >> 4) != STB_WEAK || type == RELOC_COPY)
        die("undefined symbol", name);
    return 0;
}

static struct dl_tls_module *require_tls(const struct object *o)
{
    if (!o->tls)
        die("TLS reference to an object without a TLS segment", o->name);
    return o->tls;
}

/* COPY must run after every library's ordinary relocations: a copied
 * data object may itself contain relocated pointers. A lazily bound
 * object keeps its JUMP_SLOT entries pointing into its own procedure
 * linkage table, rebased, and hands the table to _dl_runtime_resolve. */
static void apply(struct object *o, const struct rela *table, size_t bytes, int copies)
{
    for (size_t i = 0; i < bytes / sizeof *table; i++) {
        const struct rela *r = &table[i];
        uint32_t type = (uint32_t)r->r_info;
        if (type == RELOC_NONE || (type == RELOC_COPY) != copies)
            continue;
        uintptr_t where = address(o, r->r_offset);
        if (type == RELOC_RELATIVE) {
            require_range(o, where, sizeof(uint64_t), PF_W);
            *(uint64_t *)where = o->base + (uint64_t)r->r_addend;
            continue;
        }
        if (type == RELOC_JUMP_SLOT && o->lazy && table == o->jmprel) {
            require_range(o, where, sizeof(uint64_t), PF_W);
            *(uint64_t *)where += o->base;
            continue;
        }
        if (type == RELOC_TLS_DTPMOD || type == RELOC_TLS_DTPREL || type == RELOC_TLS_TPREL) {
            struct definition def;
            int found = resolve(o, r, type, &def);
            require_range(o, where, sizeof(uint64_t), PF_W);
            uint64_t value = 0;
            if (found && type == RELOC_TLS_DTPMOD) {
                value = (uint64_t)(uintptr_t)require_tls(def.object);
            } else if (found && type == RELOC_TLS_DTPREL) {
                value = def.symbol->st_value + (uint64_t)r->r_addend;
            } else if (found) {
                const struct dl_tls_module *m = require_tls(def.object);
                if (!m->offset)
                    die("initial-exec TLS reference to an object loaded by dlopen", def.object->name);
                value = dl_tls_tprel(def.symbol->st_value + (uint64_t)r->r_addend, m->offset);
            }
            *(uint64_t *)where = value;
            continue;
        }
        if (type != RELOC_ABS64 && type != RELOC_COPY &&
            type != RELOC_GLOB_DAT && type != RELOC_JUMP_SLOT)
            die("unsupported relocation type", o->name);
        struct definition def;
        uintptr_t value = resolve(o, r, type, &def) ? symbol_address(&def) : 0;
        if (type == RELOC_COPY) {
            const struct sym *s = &o->symtab[(uint32_t)(r->r_info >> 32)];
            if (o != objects || def.symbol->st_size < s->st_size)
                die("invalid copy relocation size or owner", string_at(o, s->st_name));
            require_range(o, where, s->st_size, PF_W);
            require_range(def.object, value, s->st_size, PF_R);
            dl_memcpy((void *)where, (const void *)value, s->st_size);
        } else {
            require_range(o, where, sizeof(uint64_t), PF_W);
            *(uint64_t *)where = value + (type == RELOC_ABS64 ? (uint64_t)r->r_addend : 0);
        }
    }
    if (o->lazy && table == o->jmprel && bytes) {
        o->pltgot[1] = (uintptr_t)o;
        o->pltgot[2] = (uintptr_t)_dl_runtime_resolve;
    }
}

/* Lazy binding: the procedure linkage table of o pushed the index of
 * one JUMP_SLOT relocation. Resolve it, store the address so the next
 * call goes straight through, and return it to the trampoline. */
uintptr_t _dl_fixup(struct object *o, size_t index)
{
    dl_lock();
    if (index >= o->pltrelsz / sizeof(struct rela))
        die("invalid lazy relocation index", o->name);
    const struct rela *r = &o->jmprel[index];
    if ((uint32_t)r->r_info != RELOC_JUMP_SLOT)
        die("lazy relocation is not a jump slot", o->name);
    struct definition def;
    uintptr_t value = resolve(o, r, RELOC_JUMP_SLOT, &def) ? symbol_address(&def) : 0;
    uintptr_t where = address(o, r->r_offset);
    require_range(o, where, sizeof(uint64_t), PF_W);
    *(uint64_t *)where = value;
    dl_unlock();
    return value;
}

static int prot_of(uint32_t flags)
{
    return ((flags & PF_R) ? PROT_READ : 0) | ((flags & PF_W) ? PROT_WRITE : 0) |
           ((flags & PF_X) ? PROT_EXEC : 0);
}

static const char *basename_of(const char *name)
{
    const char *b = name;
    for (const char *p = name; *p; p++)
        if (*p == '/')
            b = p + 1;
    return b;
}

/* A loaded object with this name, or with the same basename when either
 * side is a path: an object opened by path is the same object as the
 * library the loader found by that basename. */
static struct object *find_loaded(const char *name, const struct scope *pending)
{
    const char *b = basename_of(name);
    for (struct object *o = objects; o; o = o->next)
        if (o != objects && dl_strcmp(basename_of(o->name), b) == 0)
            return o;
    if (pending)
        for (size_t i = 0; i < pending->count; i++)
            if (dl_strcmp(basename_of(pending->objects[i]->name), b) == 0)
                return pending->objects[i];
    return NULL;
}

static void read_at(long fd, uint64_t offset, void *buffer, size_t bytes, const char *name)
{
    if (sys(SYS_lseek, fd, (long)offset, SEEK_SET, 0, 0, 0) < 0)
        die("cannot seek in library", name);
    unsigned char *p = buffer;
    while (bytes) {
        long n = sys(SYS_read, fd, (long)p, (long)bytes, 0, 0, 0);
        if (n <= 0)
            die("truncated or unreadable library", name);
        p += n;
        bytes -= (size_t)n;
    }
}

static void map_at(uintptr_t start, size_t bytes, int prot, long fd, uint64_t offset,
                   const char *name)
{
    int flags = MAP_PRIVATE | MAP_FIXED | (fd < 0 ? MAP_ANONYMOUS : 0);
    long r = sys(SYS_mmap, (long)start, (long)bytes, prot, flags, fd, (long)offset);
    if (r < 0 || (uintptr_t)r != start)
        die("cannot map library segment", name);
}

static void protect(uintptr_t start, size_t bytes, int prot, const char *name)
{
    if (bytes && sys(SYS_mprotect, (long)start, (long)bytes, prot, 0, 0, 0) < 0)
        die("cannot protect library segment", name);
}

/* Map a library by its soname from the library directories, or by path
 * when the name holds a slash (dlopen only). The record is returned
 * uncommitted; the caller adds it to the loaded list or, on failure,
 * unload_object releases what was mapped. */
static struct object *load_library(const char *name, int allow_path)
{
    size_t n = dl_strlen(name);
    if (!n)
        die("empty library name", NULL);
    int is_path = 0;
    for (size_t i = 0; i < n; i++)
        if (name[i] == '/')
            is_path = 1;
    if (is_path && !allow_path)
        die("library names must be basenames", name);
    char *path = NULL;
    long fd = -1;
    if (is_path) {
        path = dl_alloc(n + 1);
        dl_memcpy(path, name, n + 1);
        fd = sys(SYS_open, (long)path, O_RDONLY, 0, 0, 0, 0);
    }
    size_t ndirs = sizeof lib_dirs / sizeof lib_dirs[0];
    for (size_t d = 0; !is_path && d <= ndirs && fd < 0; d++) {
        const char *dir = d < ndirs ? lib_dirs[d] : home_lib;
        if (!dir)
            break;
        size_t dn = dl_strlen(dir);
        path = dl_alloc(dn + n + 1);
        dl_memcpy(path, dir, dn);
        dl_memcpy(path + dn, name, n + 1);
        fd = sys(SYS_open, (long)path, O_RDONLY, 0, 0, 0, 0);
    }
    if (fd < 0)
        die("cannot open library", name);
    struct stat st;
    if (sys(SYS_fstat, fd, (long)&st, 0, 0, 0, 0) < 0 || st.st_size < (long)sizeof(struct ehdr))
        die("invalid library file size", path);
    struct ehdr eh;
    read_at(fd, 0, &eh, sizeof eh, path);
    if (eh.e_ident[0] != 0x7f || eh.e_ident[1] != 'E' || eh.e_ident[2] != 'L' || eh.e_ident[3] != 'F' ||
        eh.e_ident[4] != 2 || eh.e_ident[5] != 1 || eh.e_ident[6] != 1 ||
        eh.e_type != ET_DYN || eh.e_machine != LD_ARCH_ELF_MACHINE || eh.e_version != 1 || eh.e_ehsize != sizeof eh ||
        eh.e_phentsize != sizeof(struct phdr) || !eh.e_phnum ||
        !within(eh.e_phoff, (size_t)eh.e_phnum * sizeof(struct phdr), st.st_size))
        die("invalid ELF64 shared object", path);
    /* An object opened by dlopen keeps a copy of its name, since the
     * caller may reuse its buffer for the next dlopen and find_loaded
     * compares the names. The name of a DT_NEEDED entry lives in the
     * string table of the object that needs it. */
    const char *kept = name;
    if (allow_path) {
        char *copy = is_path ? path : dl_alloc(n + 1);
        if (!is_path)
            dl_memcpy(copy, name, n + 1);
        kept = copy;
    }
    struct object *o = new_object(kept);
    loading = o;
    o->phnum = eh.e_phnum;
    struct phdr *ph = dl_alloc(o->phnum * sizeof *ph);
    read_at(fd, eh.e_phoff, ph, o->phnum * sizeof *ph, path);
    o->phdr = ph;

    uintptr_t lo = UINTPTR_MAX, hi = 0, alignment = LIB_ALIGN;
    for (size_t i = 0; i < o->phnum; i++) {
        const struct phdr *p = &ph[i];
        if (p->p_type != PT_LOAD)
            continue;
        if (p->p_filesz > p->p_memsz || !within(p->p_offset, p->p_filesz, st.st_size) ||
            !within(p->p_vaddr, p->p_memsz, LIB_LIMIT - LIB_ARENA) ||
            p->p_vaddr % PAGE != p->p_offset % PAGE ||
            (p->p_align > 1 && (!power_of_two(p->p_align) || p->p_align > LIB_LIMIT - LIB_ARENA ||
                               p->p_vaddr % p->p_align != p->p_offset % p->p_align)))
            die("invalid load segment", name);
        if (!p->p_memsz)
            continue;
        uintptr_t start = ALIGN_DOWN(p->p_vaddr, PAGE), end = ALIGN_UP(p->p_vaddr + p->p_memsz, PAGE);
        for (size_t j = 0; j < i; j++) {
            const struct phdr *q = &ph[j];
            if (q->p_type == PT_LOAD && q->p_memsz && start < ALIGN_UP(q->p_vaddr + q->p_memsz, PAGE) &&
                ALIGN_DOWN(q->p_vaddr, PAGE) < end)
                die("overlapping load segments", name);
        }
        if (start < lo) lo = start;
        if (end > hi) hi = end;
        if (p->p_align > alignment) alignment = p->p_align;
    }
    if (hi <= lo)
        die("shared object without loadable segments", name);
    o->base = ALIGN_UP(next_base, alignment);
    if (!within(o->base, hi + PAGE, LIB_LIMIT))
        die("library address space exhausted", name);
    next_base = ALIGN_UP(o->base + hi + PAGE, LIB_ALIGN);
    map_at(o->base + lo, hi - lo, PROT_NONE, -1, 0, name);
    o->map_start = o->base + lo;
    o->map_size = hi - lo;
    for (size_t i = 0; i < o->phnum; i++) {
        const struct phdr *p = &ph[i];
        if (p->p_type != PT_LOAD || !p->p_memsz)
            continue;
        uintptr_t start = o->base + ALIGN_DOWN(p->p_vaddr, PAGE);
        uintptr_t file_end = o->base + p->p_vaddr + p->p_filesz;
        uintptr_t mem_end = o->base + p->p_vaddr + p->p_memsz;
        int prot = prot_of(p->p_flags);
        int tail = p->p_filesz && mem_end > file_end && file_end % PAGE;
        size_t file_bytes = ALIGN_UP(file_end, PAGE) - start;
        if (p->p_filesz)
            map_at(start, file_bytes, tail ? prot | PROT_WRITE : prot, fd, ALIGN_DOWN(p->p_offset, PAGE), name);
        /* A completely zero-filled segment may begin partway into a page.
         * Map that first page as well; there is no file mapping to own it. */
        uintptr_t anon_start = p->p_filesz ? ALIGN_UP(file_end, PAGE) : start;
        if (ALIGN_UP(mem_end, PAGE) > anon_start)
            map_at(anon_start, ALIGN_UP(mem_end, PAGE) - anon_start, prot, -1, 0, name);
        if (tail) {
            uintptr_t tail_end = ALIGN_UP(file_end, PAGE);
            if (tail_end > mem_end) tail_end = mem_end;
            dl_memset((void *)file_end, 0, tail_end - file_end);
            if (!(prot & PROT_WRITE))
                protect(start, file_bytes, prot, name);
        }
    }
    sys(SYS_close, fd, 0, 0, 0, 0, 0);
    parse_dynamic(o);
    loading = NULL;
    return o;
}

/* The RELRO range must start in a readable load segment. GNU ld rounds
 * its end up to a page boundary, so when no ordinary writable data
 * follows it, the range ends in the padding of the segment's last page,
 * which is mapped with the segment. */
static void require_relro(const struct object *o, uintptr_t addr, size_t bytes)
{
    uint64_t offset = addr - o->base;
    for (size_t i = 0; i < o->phnum; i++) {
        const struct phdr *p = &o->phdr[i];
        if (p->p_type != PT_LOAD || !(p->p_flags & PF_R) || offset < p->p_vaddr || offset - p->p_vaddr >= p->p_memsz)
            continue;
        uint64_t mapped = ALIGN_UP(p->p_vaddr + p->p_memsz, PAGE) - p->p_vaddr;
        if (within(offset - p->p_vaddr, bytes, mapped))
            return;
    }
    die("ELF range outside load segments", o->name);
}

static void protect_relro(struct object *o)
{
    for (size_t i = 0; i < o->phnum; i++) {
        const struct phdr *p = &o->phdr[i];
        if (p->p_type != PT_GNU_RELRO || !p->p_memsz)
            continue;
        uintptr_t addr = address(o, p->p_vaddr);
        require_relro(o, addr, p->p_memsz);
        uintptr_t start = ALIGN_DOWN(addr, PAGE), end = ALIGN_DOWN(addr + p->p_memsz, PAGE);
        /* Do not protect the last partial page: ordinary writable data may
         * share it. GNU linkers end RELRO on a page boundary when possible. */
        protect(start, end - start, PROT_READ, o->name);
    }
}

static void call_init(uintptr_t fn, int argc, char **argv, char **envp)
{
    if (!fn || fn == UINTPTR_MAX)
        return;
    for (const struct object *o = objects; o; o = o->next) {
        if (contains(o, fn, 1, PF_X)) {
            ((void (*)(int, char **, char **))fn)(argc, argv, envp);
            return;
        }
    }
    die("initializer outside executable segments", NULL);
}

static void call_fini(uintptr_t fn)
{
    if (!fn || fn == UINTPTR_MAX)
        return;
    for (const struct object *o = objects; o; o = o->next) {
        if (contains(o, fn, 1, PF_X)) {
            ((void (*)(void))fn)();
            return;
        }
    }
    die("finalizer outside executable segments", NULL);
}

/* INIT and INIT_ARRAY of root and everything it needs, in dependency
 * order, with array entries in their linker-assigned priority order.
 * Objects initialized earlier, by startup or another dlopen, are skipped. */
static void initialize_from(struct object *root)
{
    if (root->init_state)
        return;
    struct object *o = root;
    o->init_state = 1;
    o->init_parent = NULL;
    o->next_needed = 0;
    while (o) {
        struct object *dependency = NULL;
        while (o->next_needed < o->ndyn) {
            const struct dyn *d = &o->dyn[o->next_needed++];
            if (d->d_tag != DT_NEEDED)
                continue;
            struct object *child = find_loaded(string_at(o, d->d_val), NULL);
            if (child && !child->init_state) {
                dependency = child;
                break;
            }
        }
        if (dependency) {
            dependency->init_parent = o;
            dependency->init_state = 1;
            dependency->next_needed = 0;
            o = dependency;
            continue;
        }
        o->init_state = 2;
        o->fini_next = fini_head;
        fini_head = o;
        call_init(o->init, argc_saved, argv_saved, envp_saved);
        for (size_t i = 0; i < o->init_size / sizeof(uintptr_t); i++)
            call_init(o->init_array[i], argc_saved, argv_saved, envp_saved);
        o = o->init_parent;
    }
}

/* Called by libc after errno, allocation, environ and stdio are usable.
 * PREINIT belongs to the executable. */
void _dl_initialize(int argc, char **argv, char **envp)
{
    argc_saved = argc;
    argv_saved = argv;
    envp_saved = envp;
    for (size_t i = 0; i < objects->preinit_size / sizeof(uintptr_t); i++)
        call_init(objects->preinit_array[i], argc, argv, envp);
    initialize_from(objects);
}

static void finalize_object(struct object *o)
{
    for (size_t i = o->fini_size / sizeof(uintptr_t); i; i--)
        call_fini(o->fini_array[i - 1]);
    call_fini(o->fini);
}

/* libc registers this before constructors can register atexit handlers.
 * Ordinary exit runs those handlers first, then reverses initialization.
 * Remove a record before invoking user code so recursive exit cannot run
 * the same object's finalizers twice. _exit intentionally bypasses them. */
void _dl_finalize(void)
{
    while (fini_head) {
        struct object *o = fini_head;
        fini_head = o->fini_next;
        finalize_object(o);
    }
}

/* ---- thread local storage ---- */

/* The static layout (dl_tls_place): the program's block is placed first,
 * where the linker assumed it for its local-exec accesses, and each further
 * initial object's block follows. */
static void layout_static_tls(void)
{
    size_t total = DL_TLS_TCB_SIZE, align = 16;
    for (struct object *o = objects; o; o = o->next) {
        struct dl_tls_module *m = o->tls;
        if (!m)
            continue;
        if (m->align > align)
            align = m->align;
        m->offset = dl_tls_place(&total, m->memsz, m->align);
    }
    interface.static_tls_size = total;
    interface.static_tls_align = align;
}

static void tls_setup(void *tcb)
{
    dl_lock();
    for (const struct object *o = objects; o; o = o->next) {
        const struct dl_tls_module *m = o->tls;
        if (!m || !m->offset)
            continue;
        unsigned char *block = dl_tls_block(tcb, m->offset);
        dl_memcpy(block, (const void *)m->image, m->filesz);
        dl_memset(block + m->filesz, 0, m->memsz - m->filesz);
    }
    ((struct dl_tcb *)tcb)->dtv = NULL;
    dl_unlock();
}

static size_t allocate_slot(struct dl_tls_module *m)
{
    for (size_t i = 0; i < dynamic_count; i++) {
        if (!dynamic_modules[i]) {
            dynamic_modules[i] = m;
            return i + 1;
        }
    }
    if (dynamic_count == dynamic_capacity) {
        size_t capacity = dynamic_capacity ? 2 * dynamic_capacity : 8;
        struct dl_tls_module **grown = dl_alloc(capacity * sizeof *grown);
        for (size_t i = 0; i < dynamic_count; i++)
            grown[i] = dynamic_modules[i];
        dynamic_modules = grown;
        dynamic_capacity = capacity;
    }
    dynamic_modules[dynamic_count++] = m;
    return dynamic_count;
}

/* The slow path of __tls_get_addr, for a module loaded by dlopen: the
 * thread's vector grows to cover the slot, and a block is allocated and
 * initialized on the first access, or again when the slot was reused by
 * a later module (the generation differs). */
static void *tls_get_addr(struct dl_tls_module *m, size_t offset)
{
    struct dl_tcb *tcb = current_tcb();
    if (m->offset)
        return dl_tls_block(tcb, m->offset) + offset;
    dl_lock();
    struct dl_dtv *dtv = tcb->dtv;
    if (!dtv || dtv->count <= m->index) {
        size_t count = dtv ? 2 * dtv->count : 8;
        if (count <= m->index)
            count = m->index + 1;
        struct dl_dtv *grown = interface.alloc(sizeof *grown + count * sizeof grown->entries[0]);
        if (!grown)
            die("cannot allocate the thread's TLS vector", NULL);
        dl_memset(grown, 0, sizeof *grown + count * sizeof grown->entries[0]);
        grown->count = count;
        if (dtv) {
            dl_memcpy(grown->entries, dtv->entries, dtv->count * sizeof grown->entries[0]);
            interface.free(dtv);
        }
        tcb->dtv = dtv = grown;
    }
    struct dl_dtv_entry *e = &dtv->entries[m->index];
    if (!e->block || e->generation != m->generation) {
        if (e->raw)
            interface.free(e->raw);
        e->raw = interface.alloc(m->memsz + m->align);
        if (!e->raw)
            die("cannot allocate a TLS block", NULL);
        e->block = (void *)ALIGN_UP((uintptr_t)e->raw, m->align);
        dl_memcpy(e->block, (const void *)m->image, m->filesz);
        dl_memset((unsigned char *)e->block + m->filesz, 0, m->memsz - m->filesz);
        e->generation = m->generation;
    }
    void *p = (unsigned char *)e->block + offset;
    dl_unlock();
    return p;
}

static void tls_free(void *tcb_pointer)
{
    struct dl_tcb *tcb = tcb_pointer;
    struct dl_dtv *dtv = tcb->dtv;
    if (!dtv)
        return;
    dl_lock();
    for (size_t i = 0; i < dtv->count; i++)
        if (dtv->entries[i].raw)
            interface.free(dtv->entries[i].raw);
    interface.free(dtv);
    tcb->dtv = NULL;
    dl_unlock();
}

/* ---- dlopen, dlsym, dlclose ---- */

static const char *dl_error(void)
{
    if (!error_set)
        return NULL;
    error_set = 0;
    return error_text;
}

static void set_error(const char *what, const char *name)
{
    size_t used = 0;
    append(error_text, sizeof error_text, &used, what);
    if (name) {
        append(error_text, sizeof error_text, &used, ": ");
        append(error_text, sizeof error_text, &used, name);
    }
    error_set = 1;
}

/* Release an object that is not, or no longer, in the loaded list. */
static void unload_object(struct object *o)
{
    if (o->map_size)
        sys(SYS_munmap, (long)o->map_start, (long)o->map_size, 0, 0, 0, 0);
    if (o->tls && o->tls->index) {
        dynamic_modules[o->tls->index - 1] = NULL;
        o->tls->index = 0;
    }
    o->map_size = 0;
    o->next = free_objects;
    free_objects = o;
}

static int valid_handle(const void *handle)
{
    for (const struct object *o = objects; o; o = o->next)
        if (o == handle)
            return 1;
    return 0;
}

/* Load name and what it needs as one group with its own scope, relocate
 * and initialize the new objects, and return the group's leader. A name
 * already loaded returns the existing object with another reference. */
static void *dl_open(const char *name, int mode)
{
    dl_lock();
    if (!name) {
        dl_unlock();
        return objects;
    }
    struct object *found = find_loaded(name, NULL);
    if (found) {
        if (!found->permanent)
            for (size_t i = 0; i < found->local->count; i++)
                found->local->objects[i]->refs++;
        if ((mode & DL_GLOBAL) && !found->global) {
            scope_add(&global_scope, found);
            found->global = 1;
        }
        dl_unlock();
        return found;
    }
    struct scope *group = dl_alloc(sizeof *group);
    volatile size_t loaded = 0;     /* the first loaded objects of group are new */
    int depth = dl_lock_state.depth;
    recovering = 1;
    if (_dl_setjmp(recover)) {
        for (size_t i = 0; i < loaded; i++)
            unload_object(group->objects[i]);
        if (loading) {
            unload_object(loading);
            loading = NULL;
        }
        recovering = 0;
        dl_lock_state.depth = depth;
        dl_unlock();
        return NULL;
    }
    scope_add(group, load_library(name, 1));
    loaded = 1;
    /* Breadth first, as at startup: the group holds the new objects
     * first, then the loaded ones they depend on. */
    for (size_t i = 0; i < loaded; i++) {
        struct object *o = group->objects[i];
        for (size_t k = 0; k < o->ndyn; k++) {
            if (o->dyn[k].d_tag != DT_NEEDED)
                continue;
            const char *needed = string_at(o, o->dyn[k].d_val);
            if (find_loaded(needed, group))
                continue;
            scope_add(group, load_library(needed, 0));
            loaded++;
        }
    }
    size_t new_count = loaded;
    for (size_t i = 0; i < new_count; i++) {
        struct object *o = group->objects[i];
        for (size_t k = 0; k < o->ndyn; k++) {
            if (o->dyn[k].d_tag != DT_NEEDED)
                continue;
            struct object *dep = find_loaded(string_at(o, o->dyn[k].d_val), group);
            if (dep)
                scope_add(group, dep);
        }
    }
    for (size_t i = 0; i < new_count; i++) {
        struct object *o = group->objects[i];
        o->local = group;
        o->lazy = o->pltgot && o->pltrelsz && !o->bind_now && !(mode & DL_NOW);
        if (o->tls) {
            o->tls->offset = 0;
            o->tls->index = allocate_slot(o->tls);
            o->tls->generation = ++tls_generation;
        }
    }
    for (int copies = 0; copies <= 1; copies++) {
        for (size_t i = 0; i < new_count; i++) {
            apply(group->objects[i], group->objects[i]->rela, group->objects[i]->relasz, copies);
            apply(group->objects[i], group->objects[i]->jmprel, group->objects[i]->pltrelsz, copies);
        }
    }
    for (size_t i = 0; i < new_count; i++)
        protect_relro(group->objects[i]);
    recovering = 0;
    for (size_t i = 0; i < new_count; i++)
        commit_object(group->objects[i]);
    for (size_t i = 0; i < group->count; i++) {
        struct object *o = group->objects[i];
        o->refs++;
        if ((mode & DL_GLOBAL) && !o->global) {
            scope_add(&global_scope, o);
            o->global = 1;
        }
    }
    initialize_from(group->objects[0]);
    dl_unlock();
    return group->objects[0];
}

static void *dl_sym(void *handle, const char *name)
{
    dl_lock();
    if (handle && !valid_handle(handle)) {
        set_error("invalid handle", name);
        dl_unlock();
        return NULL;
    }
    void *result = NULL;
    int depth = dl_lock_state.depth;
    recovering = 1;
    if (_dl_setjmp(recover)) {
        recovering = 0;
        dl_lock_state.depth = depth;
        dl_unlock();
        return NULL;
    }
    struct definition def = { NULL, NULL };
    if (!handle) {
        lookup(name, NULL, NULL, &def);
    } else {
        const struct object *o = handle;
        const struct sym *s = lookup_in(o, name);
        if (s) {
            def.object = o;
            def.symbol = s;
        } else {
            for (size_t i = 0; i < o->local->count && !def.symbol; i++) {
                s = lookup_in(o->local->objects[i], name);
                if (s) {
                    def.object = o->local->objects[i];
                    def.symbol = s;
                }
            }
        }
    }
    if (!def.symbol)
        set_error("undefined symbol", name);
    else if ((def.symbol->st_info & 15) == STT_TLS)
        result = tls_get_addr(require_tls(def.object), def.symbol->st_value);
    else
        result = (void *)symbol_address(&def);
    recovering = 0;
    dl_unlock();
    return result;
}

/* Drop one reference of the handle's group and unload the members no
 * group refers to any more: their finalizers run in the reverse of the
 * order they initialized in, their mappings go, and a TLS slot they
 * held is released, the blocks of other threads with the next thread
 * exit or the slot's reuse. */
static int dl_close(void *handle)
{
    dl_lock();
    struct object *o = handle;
    if (!valid_handle(o)) {
        set_error("invalid handle", NULL);
        dl_unlock();
        return -1;
    }
    if (o->permanent) {
        dl_unlock();
        return 0;
    }
    struct scope *group = o->local;
    for (size_t i = 0; i < group->count; i++)
        if (group->objects[i]->refs)
            group->objects[i]->refs--;
    for (struct object **p = &fini_head; *p;) {
        struct object *f = *p;
        if (!f->permanent && !f->refs) {
            *p = f->fini_next;
            f->init_state = 0;
            finalize_object(f);
        } else {
            p = &f->fini_next;
        }
    }
    for (size_t i = 0; i < group->count; i++) {
        struct object *m = group->objects[i];
        if (m->permanent || m->refs)
            continue;
        scope_remove(&global_scope, m);
        remove_object(m);
        unload_object(m);
    }
    dl_unlock();
    return 0;
}

/* Before this finishes, only PC-relative code and stack locals are safe.
 * The loader's own build uses -Bsymbolic and has RELATIVE relocations only;
 * it does not use the general object parser or allocation machinery here. */
static void relocate_self(uintptr_t base)
{
    const struct rela *rel = NULL;
    size_t bytes = 0;
    for (const struct dyn *d = _DYNAMIC; d->d_tag != DT_NULL; d++) {
        if (d->d_tag == DT_RELA) rel = (const void *)(base + d->d_val);
        if (d->d_tag == DT_RELASZ) bytes = d->d_val;
    }
    for (size_t i = 0; i < bytes / sizeof *rel; i++) {
        if ((uint32_t)rel[i].r_info == RELOC_NONE)
            continue;
        if ((uint32_t)rel[i].r_info != RELOC_RELATIVE)
            die("unsupported loader self relocation", NULL);
        *(uintptr_t *)(base + rel[i].r_offset) = base + (uint64_t)rel[i].r_addend;
    }
}

/* Hand the interface to the C library, when the program uses one: the
 * pointer variable __dl_interface of libc receives the record. */
static void publish_interface(void)
{
    interface.version = DL_INTERFACE_VERSION;
    interface.tls_setup = tls_setup;
    interface.tls_get_addr = tls_get_addr;
    interface.tls_free = tls_free;
    interface.open = dl_open;
    interface.sym = dl_sym;
    interface.close = dl_close;
    interface.error = dl_error;
    struct definition def;
    if (!lookup("__dl_interface", NULL, NULL, &def))
        return;
    uintptr_t where = symbol_address(&def);
    require_range(def.object, where, sizeof(void *), PF_W);
    *(struct dl_interface **)where = &interface;
}

uintptr_t _dl_main(uintptr_t *sp)
{
    uintptr_t argc = sp[0];
    uintptr_t *aux = sp + argc + 2;
    while (*aux)
        aux++;
    aux++;
    uintptr_t phdr = 0, phnum = 0, phent = 0, base = 0, entry = 0, is_secure = 0;
    for (; aux[0] != AT_NULL; aux += 2) {
        switch (aux[0]) {
        case AT_SECURE: is_secure = aux[1]; break;
        case AT_PHDR: phdr = aux[1]; break;
        case AT_PHENT: phent = aux[1]; break;
        case AT_PHNUM: phnum = aux[1]; break;
        case AT_BASE: base = aux[1]; break;
        case AT_ENTRY: entry = aux[1]; break;
        default: break;
        }
    }
    relocate_self(base);
    if (!base || !phdr || !phnum || phnum > UINT16_MAX || phent != sizeof(struct phdr) || !entry)
        die("invalid program auxiliary vector", NULL);
    secure = is_secure != 0;
    for (char **env = (char **)(sp + argc + 2); !secure && *env; env++) {
        const char *e = *env;
        if (e[0] == 'H' && e[1] == 'O' && e[2] == 'M' && e[3] == 'E' && e[4] == '=' && e[5] == '/') {
            size_t hn = dl_strlen(e + 5);
            home_lib = dl_alloc(hn + sizeof "/.local/lib/");
            dl_memcpy(home_lib, e + 5, hn);
            dl_memcpy(home_lib + hn, "/.local/lib/", sizeof "/.local/lib/");
        }
    }
    struct object *prog = new_object("program");
    commit_object(prog);
    prog->phdr = (const void *)phdr;
    prog->phnum = phnum;
    require_range(prog, phdr, phnum * sizeof(struct phdr), PF_R);
    require_range(prog, entry, 1, PF_X);
    parse_dynamic(prog);
    /* Appending to the list while walking it gives breadth-first symbol
     * scope, without relocating object records or imposing an object cap. */
    for (struct object *o = objects; o; o = o->next) {
        for (size_t i = 0; i < o->ndyn; i++) {
            if (o->dyn[i].d_tag != DT_NEEDED)
                continue;
            const char *name = string_at(o, o->dyn[i].d_val);
            if (!find_loaded(name, NULL))
                commit_object(load_library(name, 0));
        }
    }
    for (struct object *o = objects; o; o = o->next) {
        scope_add(&global_scope, o);
        o->local = &global_scope;
        o->global = 1;
        o->permanent = 1;
        o->refs = 1;
        o->lazy = o->pltgot && o->pltrelsz && !o->bind_now;
    }
    layout_static_tls();
    for (int copies = 0; copies <= 1; copies++) {
        for (struct object *o = objects; o; o = o->next) {
            apply(o, o->rela, o->relasz, copies);
            apply(o, o->jmprel, o->pltrelsz, copies);
        }
    }
    publish_interface();
    for (struct object *o = objects; o; o = o->next)
        protect_relro(o);
    return entry;
}
