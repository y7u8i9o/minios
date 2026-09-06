/* MiniOS ELF64 dynamic loader, /lib/ld.so.
 *
 * Startup has three distinct phases: relocate this freestanding loader,
 * map and relocate the program's dependency graph, then let libc initialize
 * its thread state and standard streams before calling ELF initializers.
 * start.S passes the initialization and finalization callbacks to crt0.
 *
 * Metadata belongs to the process for its entire lifetime. A small mmap
 * arena keeps object addresses stable as dependencies are discovered; no
 * libc allocation, TLS, or locks are used here. Loading happens on the
 * initial thread, before any application code can create other threads.
 * Runtime loading with dlopen is deliberately not part of this interface.
 */
#include <stdint.h>
#include <stddef.h>
#include <syscall_nums.h>
#include <minios/abi.h>

#define PAGE 4096UL
#define ALIGN_DOWN(x, a) ((x) & ~((a) - 1))
#define ALIGN_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))
/* The system libraries, then the package prefix (minios/local.h). */
static const char *const lib_dirs[] = { "/lib/", "/home/.local/lib/" };
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
#define DT_INIT_ARRAY 25
#define DT_FINI_ARRAY 26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28
#define DT_PREINIT_ARRAY 32
#define DT_PREINIT_ARRAYSZ 33
#define DT_RELRSZ 35
#define DT_RELR 36
#define DT_GNU_HASH 0x6ffffef5
#define DT_VERDEFNUM 0x6ffffffd
#define DT_VERNEEDNUM 0x6fffffff
#define STB_GLOBAL 1
#define STB_WEAK 2
#define STT_TLS 6
#define STT_GNU_IFUNC 10
#define STV_INTERNAL 1
#define STV_HIDDEN 2
#define STV_PROTECTED 3
#define SHN_UNDEF 0
#define SHN_ABS 0xfff1
#define R_X86_64_NONE 0
#define R_X86_64_64 1
#define R_X86_64_COPY 5
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE 8
#define AT_NULL 0
#define AT_PHDR 3
#define AT_PHENT 4
#define AT_PHNUM 5
#define AT_BASE 7
#define AT_ENTRY 9

/* All addresses in these records are relocated process addresses. Program
 * headers remain available for checking every table, symbol and write. */
struct object {
    const char *name;
    uintptr_t base;
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
    uintptr_t init, fini;
    const uintptr_t *preinit_array, *init_array, *fini_array;
    size_t preinit_size, init_size, fini_size;
    struct object *next;
    /* Iterative DFS state for dependency-first initialization. Objects in
     * a cycle are visited once. fini_next records the actual call order. */
    struct object *init_parent, *fini_next;
    size_t next_needed;
    unsigned init_state;
};

static struct object *objects, *last_object, *fini_head;
static uintptr_t next_base = LIB_ARENA;
static unsigned char *arena;
static size_t arena_left;
extern struct dyn _DYNAMIC[] __attribute__((visibility("hidden")));

static long sys(long nr, long a, long b, long c, long d, long e, long f)
{
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    long ret;
    __asm__ volatile("syscall" : "=a"(ret) : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
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

__attribute__((noreturn)) static void die(const char *what, const char *name)
{
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
    struct object *o = dl_alloc(sizeof *o);
    o->name = name;
    if (last_object)
        last_object->next = o;
    else
        objects = o;
    last_object = o;
    return o;
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

/* Only the formats which this loader understands may reach relocation.
 * Unsupported TLS, REL/RELR, version requirements and text relocations
 * receive diagnostics instead of being silently ignored. */
static void parse_dynamic(struct object *o)
{
    const struct phdr *dynamic = NULL;
    for (size_t i = 0; i < o->phnum; i++) {
        const struct phdr *p = &o->phdr[i];
        if (p->p_type == PT_TLS)
            die("ELF TLS is not supported", o->name);
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
 * value is zero. The executable precedes its breadth-first dependencies. */
static int lookup(const char *name, int skip_program, struct definition *out)
{
    for (const struct object *o = skip_program ? objects->next : objects; o; o = o->next) {
        const struct sym *s = lookup_in(o, name);
        if (s) {
            out->object = o;
            out->symbol = s;
            return 1;
        }
    }
    return 0;
}

static uintptr_t symbol_address(const struct definition *def)
{
    const struct sym *s = def->symbol;
    unsigned type = s->st_info & 15;
    if (type == STT_TLS || type == STT_GNU_IFUNC)
        die("TLS and IFUNC symbols are not supported", def->object->name);
    if (s->st_shndx == SHN_ABS)
        return s->st_value;
    uintptr_t value = address(def->object, s->st_value);
    require_range(def->object, value, s->st_size, 0);
    return value;
}

/* COPY must run after every library's ordinary relocations: a copied
 * data object may itself contain relocated pointers. A local or protected
 * definition binds within its own object; other definitions interpose in
 * load order, including the executable's copy-relocation destination. */
static void apply(struct object *o, const struct rela *table, size_t bytes, int copies)
{
    for (size_t i = 0; i < bytes / sizeof *table; i++) {
        const struct rela *r = &table[i];
        uint32_t type = (uint32_t)r->r_info;
        if (type == R_X86_64_NONE || (type == R_X86_64_COPY) != copies)
            continue;
        uintptr_t where = address(o, r->r_offset);
        if (type == R_X86_64_RELATIVE) {
            require_range(o, where, sizeof(uint64_t), PF_W);
            *(uint64_t *)where = o->base + (uint64_t)r->r_addend;
            continue;
        }
        if (type != R_X86_64_64 && type != R_X86_64_COPY &&
            type != R_X86_64_GLOB_DAT && type != R_X86_64_JUMP_SLOT)
            die("unsupported relocation type", o->name);
        uint32_t index = (uint32_t)(r->r_info >> 32);
        if (index >= o->nsyms)
            die("relocation symbol index outside table", o->name);
        const struct sym *s = &o->symtab[index];
        const char *name = string_at(o, s->st_name);
        struct definition def = { o, s };
        uintptr_t value = 0;
        unsigned visibility = s->st_other & 3;
        if (type != R_X86_64_COPY && s->st_shndx != SHN_UNDEF &&
            ((s->st_info >> 4) == 0 || visibility != 0)) {
            value = symbol_address(&def);
        } else if (lookup(name, type == R_X86_64_COPY, &def)) {
            value = symbol_address(&def);
        } else if ((s->st_info >> 4) != STB_WEAK || type == R_X86_64_COPY) {
            die("undefined symbol", name);
        }
        if (type == R_X86_64_COPY) {
            if (o != objects || def.symbol->st_size < s->st_size)
                die("invalid copy relocation size or owner", name);
            require_range(o, where, s->st_size, PF_W);
            require_range(def.object, value, s->st_size, PF_R);
            dl_memcpy((void *)where, (const void *)value, s->st_size);
        } else {
            require_range(o, where, sizeof(uint64_t), PF_W);
            *(uint64_t *)where = value + (type == R_X86_64_64 ? (uint64_t)r->r_addend : 0);
        }
    }
}

static int prot_of(uint32_t flags)
{
    return ((flags & PF_R) ? PROT_READ : 0) | ((flags & PF_W) ? PROT_WRITE : 0) |
           ((flags & PF_X) ? PROT_EXEC : 0);
}

static struct object *find_object(const char *name)
{
    for (struct object *o = objects; o; o = o->next)
        if (dl_strcmp(o->name, name) == 0)
            return o;
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

static struct object *load_library(const char *name)
{
    size_t n = dl_strlen(name);
    if (!n)
        die("empty library name", NULL);
    for (size_t i = 0; i < n; i++)
        if (name[i] == '/')
            die("library names must be basenames", name);
    char *path = NULL;
    long fd = -1;
    for (size_t d = 0; d < sizeof lib_dirs / sizeof lib_dirs[0] && fd < 0; d++) {
        size_t dn = dl_strlen(lib_dirs[d]);
        path = dl_alloc(dn + n + 1);
        dl_memcpy(path, lib_dirs[d], dn);
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
        eh.e_type != ET_DYN || eh.e_machine != 62 || eh.e_version != 1 || eh.e_ehsize != sizeof eh ||
        eh.e_phentsize != sizeof(struct phdr) || !eh.e_phnum ||
        !within(eh.e_phoff, (size_t)eh.e_phnum * sizeof(struct phdr), st.st_size))
        die("invalid ELF64 shared object", path);
    struct object *o = new_object(name);
    o->phnum = eh.e_phnum;
    struct phdr *ph = dl_alloc(o->phnum * sizeof *ph);
    read_at(fd, eh.e_phoff, ph, o->phnum * sizeof *ph, path);
    o->phdr = ph;

    uintptr_t lo = UINTPTR_MAX, hi = 0, alignment = LIB_ALIGN;
    for (size_t i = 0; i < o->phnum; i++) {
        const struct phdr *p = &ph[i];
        if (p->p_type == PT_TLS)
            die("ELF TLS is not supported", name);
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
    return o;
}

static void load_needed(void)
{
    /* Appending to the list while walking it gives breadth-first symbol
     * scope, without relocating object records or imposing an object cap. */
    for (struct object *o = objects; o; o = o->next) {
        for (size_t i = 0; i < o->ndyn; i++) {
            if (o->dyn[i].d_tag != DT_NEEDED)
                continue;
            const char *name = string_at(o, o->dyn[i].d_val);
            if (!find_object(name))
                load_library(name);
        }
    }
}

static void protect_relro(struct object *o)
{
    for (size_t i = 0; i < o->phnum; i++) {
        const struct phdr *p = &o->phdr[i];
        if (p->p_type != PT_GNU_RELRO || !p->p_memsz)
            continue;
        uintptr_t addr = address(o, p->p_vaddr);
        require_range(o, addr, p->p_memsz, PF_R);
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

/* Called by libc after errno, allocation, environ and stdio are usable.
 * PREINIT belongs to the executable; INIT and INIT_ARRAY follow dependency
 * order, with array entries in their linker-assigned priority order. */
void _dl_initialize(int argc, char **argv, char **envp)
{
    for (size_t i = 0; i < objects->preinit_size / sizeof(uintptr_t); i++)
        call_init(objects->preinit_array[i], argc, argv, envp);
    struct object *o = objects;
    o->init_state = 1;
    while (o) {
        struct object *dependency = NULL;
        while (o->next_needed < o->ndyn) {
            const struct dyn *d = &o->dyn[o->next_needed++];
            if (d->d_tag != DT_NEEDED)
                continue;
            struct object *child = find_object(string_at(o, d->d_val));
            if (child && !child->init_state) {
                dependency = child;
                break;
            }
        }
        if (dependency) {
            dependency->init_parent = o;
            dependency->init_state = 1;
            o = dependency;
            continue;
        }
        o->init_state = 2;
        o->fini_next = fini_head;
        fini_head = o;
        call_init(o->init, argc, argv, envp);
        for (size_t i = 0; i < o->init_size / sizeof(uintptr_t); i++)
            call_init(o->init_array[i], argc, argv, envp);
        o = o->init_parent;
    }
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
        for (size_t i = o->fini_size / sizeof(uintptr_t); i; i--)
            call_fini(o->fini_array[i - 1]);
        call_fini(o->fini);
    }
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
        if ((uint32_t)rel[i].r_info == R_X86_64_NONE)
            continue;
        if ((uint32_t)rel[i].r_info != R_X86_64_RELATIVE)
            die("unsupported loader self relocation", NULL);
        *(uintptr_t *)(base + rel[i].r_offset) = base + (uint64_t)rel[i].r_addend;
    }
}

uintptr_t _dl_main(uintptr_t *sp)
{
    uintptr_t argc = sp[0];
    uintptr_t *aux = sp + argc + 2;
    while (*aux)
        aux++;
    aux++;
    uintptr_t phdr = 0, phnum = 0, phent = 0, base = 0, entry = 0;
    for (; aux[0] != AT_NULL; aux += 2) {
        switch (aux[0]) {
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
    struct object *prog = new_object("program");
    prog->phdr = (const void *)phdr;
    prog->phnum = phnum;
    require_range(prog, phdr, phnum * sizeof(struct phdr), PF_R);
    require_range(prog, entry, 1, PF_X);
    parse_dynamic(prog);
    load_needed();
    for (int copies = 0; copies <= 1; copies++) {
        for (struct object *o = objects; o; o = o->next) {
            apply(o, o->rela, o->relasz, copies);
            apply(o, o->jmprel, o->pltrelsz, copies);
        }
    }
    for (struct object *o = objects; o; o = o->next)
        protect_relro(o);
    return entry;
}
