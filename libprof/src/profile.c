#include <minios/local.h>
#include <prof/profile.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>

int prof_open(void)
{
    return open("/dev/profile", O_RDWR);
}

int prof_start(int fd, pid_t pid)
{
    return ioctl(fd, PROF_START, (void *)(long)pid);
}

int prof_stop(int fd)
{
    return ioctl(fd, PROF_STOP, (void *)0);
}

int prof_set_divider(int fd, unsigned ticks)
{
    return ioctl(fd, PROF_SET_DIVIDER, (void *)(long)ticks);
}

int prof_get_stats(int fd, struct prof_stats *st)
{
    return ioctl(fd, PROF_GET_STATS, st);
}

int prof_configure(int fd, const struct prof_config *cfg)
{
    return ioctl(fd, PROF_CONFIGURE, (void *)cfg);
}

ssize_t prof_read_events(int fd, void *buf, size_t bytes)
{
    return read(fd, buf, bytes);
}

const struct prof_event *prof_event_first(const void *buf, size_t bytes)
{
    if (bytes < PROF_EVENT_HEADER)
        return NULL;
    const struct prof_event *e = buf;
    return e->size >= PROF_EVENT_HEADER && e->size <= bytes ? e : NULL;
}

const struct prof_event *prof_event_next(const void *buf, size_t bytes, const struct prof_event *e)
{
    size_t off = (size_t)((const char *)e - (const char *)buf) + e->size;
    if (off + PROF_EVENT_HEADER > bytes)
        return NULL;
    const struct prof_event *next = (const void *)((const char *)buf + off);
    if (next->size < PROF_EVENT_HEADER || off + next->size > bytes)
        return NULL;
    return next;
}

/* ---- ELF symbol tables ---- */

struct elf64_ehdr {
    unsigned char e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};

struct elf64_shdr {
    uint32_t sh_name, sh_type;
    uint64_t sh_flags, sh_addr, sh_offset, sh_size;
    uint32_t sh_link, sh_info;
    uint64_t sh_addralign, sh_entsize;
};

struct elf64_sym {
    uint32_t st_name;
    unsigned char st_info, st_other;
    uint16_t st_shndx;
    uint64_t st_value, st_size;
};

#define SHT_SYMTAB 2
#define STT_FUNC 2

static int read_at(int fd, long off, void *buf, size_t n)
{
    if (lseek(fd, off, SEEK_SET) < 0)
        return -1;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r <= 0)
            return -1;
        got += (size_t)r;
    }
    return 0;
}

static int cmp_sym(const void *a, const void *b)
{
    const struct prof_sym *x = a, *y = b;
    return x->addr < y->addr ? -1 : x->addr > y->addr ? 1 : 0;
}

struct prof_symtab *prof_symtab_load_elf(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    struct elf64_ehdr eh;
    struct prof_symtab *t = NULL;
    struct elf64_shdr *sh = NULL;
    struct elf64_sym *syms = NULL;
    if (read_at(fd, 0, &eh, sizeof eh) < 0 || memcmp(eh.e_ident, "\177ELF", 4) != 0 || eh.e_shentsize != sizeof *sh)
        goto out;
    sh = malloc((size_t)eh.e_shnum * sizeof *sh);
    if (!sh || read_at(fd, (long)eh.e_shoff, sh, (size_t)eh.e_shnum * sizeof *sh) < 0)
        goto out;
    int symidx = -1;
    for (int i = 0; i < eh.e_shnum; i++)
        if (sh[i].sh_type == SHT_SYMTAB)
            symidx = i;
    if (symidx < 0 || sh[symidx].sh_link >= eh.e_shnum)
        goto out;
    const struct elf64_shdr *symsh = &sh[symidx], *strsh = &sh[symsh->sh_link];
    size_t nsyms = symsh->sh_size / sizeof *syms;
    syms = malloc(symsh->sh_size);
    t = calloc(1, sizeof *t);
    if (!syms || !t)
        goto fail;
    t->strings = malloc(strsh->sh_size + 1);
    t->syms = malloc(nsyms * sizeof *t->syms);
    if (!t->strings || !t->syms || read_at(fd, (long)symsh->sh_offset, syms, symsh->sh_size) < 0 ||
        read_at(fd, (long)strsh->sh_offset, t->strings, strsh->sh_size) < 0)
        goto fail;
    t->strings[strsh->sh_size] = '\0';
    for (size_t i = 0; i < nsyms; i++) {
        if ((syms[i].st_info & 0xf) != STT_FUNC || !syms[i].st_value || syms[i].st_name >= strsh->sh_size)
            continue;
        t->syms[t->count].addr = syms[i].st_value;
        t->syms[t->count].size = syms[i].st_size;
        t->syms[t->count].name = t->strings + syms[i].st_name;
        t->count++;
    }
    qsort(t->syms, t->count, sizeof *t->syms, cmp_sym);
    goto out;
fail:
    prof_symtab_free(t);
    t = NULL;
out:
    free(sh);
    free(syms);
    close(fd);
    return t;
}

struct prof_symtab *prof_symtab_load_kernel(void)
{
    FILE *f = fopen("/dev/ksyms", "r");
    if (!f)
        return NULL;
    /* Slurp the text, then split it in place. */
    size_t cap = 65536, len = 0;
    char *text = malloc(cap);
    while (text) {
        size_t n = fread(text + len, 1, cap - len - 1, f);
        len += n;
        if (n == 0)
            break;
        if (len + 1 >= cap) {
            cap *= 2;
            char *bigger = realloc(text, cap);
            if (!bigger) {
                free(text);
                text = NULL;
            } else {
                text = bigger;
            }
        }
    }
    fclose(f);
    if (!text)
        return NULL;
    text[len] = '\0';
    struct prof_symtab *t = calloc(1, sizeof *t);
    size_t lines = 0;
    for (size_t i = 0; i < len; i++)
        if (text[i] == '\n')
            lines++;
    if (!t || !(t->syms = malloc((lines + 1) * sizeof *t->syms))) {
        free(text);
        free(t);
        return NULL;
    }
    t->strings = text;
    char *p = text;
    while (*p) {
        char *end;
        uint64_t addr = strtoul(p, &end, 16);
        uint64_t size = strtoul(end, &end, 16);
        while (*end == ' ')
            end++;
        char *nl = strchr(end, '\n');
        if (!nl)
            break;
        *nl = '\0';
        if (t->count < lines) {
            t->syms[t->count].addr = addr;
            t->syms[t->count].size = size;
            t->syms[t->count].name = end;
            t->count++;
        }
        p = nl + 1;
    }
    qsort(t->syms, t->count, sizeof *t->syms, cmp_sym);
    return t;
}

void prof_symtab_free(struct prof_symtab *t)
{
    if (!t)
        return;
    for (size_t i = 0; i < t->nmodules; i++) {
        if (t->modules[i].owner) {
            prof_symtab_free(t->modules[i].syms);
            free(t->modules[i].path);
        }
    }
    free(t->modules);
    free(t->syms);
    free(t->strings);
    free(t);
}

int prof_symtab_add_maps(struct prof_symtab *t, pid_t pid)
{
    if (!t)
        return 0;
    FILE *f = fopen("/dev/maps", "r");
    if (!f)
        return 0;
    int added = 0;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        int lpid;
        unsigned long start, end, offset;
        char path[256];
        if (sscanf(line, "%d %lx %lx %lx %255s", &lpid, &start, &end, &offset, path) != 5 || lpid != pid)
            continue;
        if (strncmp(path, "/usr/lib/", 9) != 0 && strncmp(path, "/usr/local/lib/", 15) != 0 &&
            strncmp(path, "/lib/", 5) != 0)
            continue;
        struct prof_module *m = realloc(t->modules, (t->nmodules + 1) * sizeof *m);
        if (!m)
            break;
        t->modules = m;
        m = &t->modules[t->nmodules];
        memset(m, 0, sizeof *m);
        m->start = start;
        m->end = end;
        m->offset = offset;
        /* Every region of one file shares the file's table. */
        for (size_t i = 0; i < t->nmodules; i++) {
            if (t->modules[i].path && strcmp(t->modules[i].path, path) == 0) {
                m->syms = t->modules[i].syms;
                m->path = t->modules[i].path;
                break;
            }
        }
        if (!m->path) {
            m->path = strdup(path);
            m->syms = prof_symtab_load_elf(path);
            m->owner = 1;
        }
        t->nmodules++;
        added++;
    }
    fclose(f);
    return added;
}

static const struct prof_sym *lookup_own(const struct prof_symtab *t, uint64_t addr, uint64_t *off);

const struct prof_sym *prof_symtab_lookup(const struct prof_symtab *t, uint64_t addr, uint64_t *off)
{
    if (!t)
        return NULL;
    const struct prof_sym *s = lookup_own(t, addr, off);
    if (s)
        return s;
    /* Inside a library the link address is the file offset of the address,
     * since the text segment of a shared object starts at offset 0. */
    for (size_t i = 0; i < t->nmodules; i++) {
        const struct prof_module *m = &t->modules[i];
        if (addr >= m->start && addr < m->end && m->syms)
            return lookup_own(m->syms, addr - m->start + m->offset, off);
    }
    return NULL;
}

static const struct prof_sym *lookup_own(const struct prof_symtab *t, uint64_t addr, uint64_t *off)
{
    if (!t || !t->count)
        return NULL;
    size_t lo = 0, hi = t->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (t->syms[mid].addr <= addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return NULL;
    const struct prof_sym *s = &t->syms[lo - 1];
    /* Return addresses after a noreturn call land between functions, so
     * attribute an address to the preceding symbol up to the next one. */
    uint64_t limit = lo < t->count ? t->syms[lo].addr : s->addr + (s->size ? s->size : 1);
    if (addr >= limit)
        return NULL;
    if (off)
        *off = addr - s->addr;
    return s;
}

/* ---- histogram ---- */

void prof_hist_init(struct prof_hist *h)
{
    memset(h, 0, sizeof *h);
}

void prof_hist_clear(struct prof_hist *h)
{
    for (size_t i = 0; i < h->count; i++)
        free(h->buckets[i].key);
    free(h->buckets);
    free(h->index);
    memset(h, 0, sizeof *h);
}

static unsigned hist_hash(const char *s)
{
    unsigned v = 2166136261u;
    while (*s) {
        v ^= (unsigned char)*s++;
        v *= 16777619u;
    }
    return v;
}

/* Build or rebuild the index over the current buckets. */
static int hist_reindex(struct prof_hist *h, size_t want)
{
    size_t cap = 64;
    while (cap < want * 2)
        cap *= 2;
    int *index = malloc(cap * sizeof *index);
    if (!index)
        return -1;
    for (size_t i = 0; i < cap; i++)
        index[i] = -1;
    for (size_t i = 0; i < h->count; i++) {
        size_t slot = hist_hash(h->buckets[i].key) & (cap - 1);
        while (index[slot] >= 0)
            slot = (slot + 1) & (cap - 1);
        index[slot] = (int)i;
    }
    free(h->index);
    h->index = index;
    h->index_cap = cap;
    h->index_stale = 0;
    return 0;
}

void prof_hist_add(struct prof_hist *h, const char *key, int kernel)
{
    prof_hist_add_weight(h, key, kernel, 1, 0);
}

void prof_hist_add_weight(struct prof_hist *h, const char *key, int kernel,
                          uint64_t weight, uint64_t extra)
{
    h->total++;
    h->weight += weight;
    if ((!h->index || h->index_stale || (h->count + 1) * 2 > h->index_cap) &&
        hist_reindex(h, h->count + 1) < 0)
        return;
    size_t slot = hist_hash(key) & (h->index_cap - 1);
    while (h->index[slot] >= 0) {
        struct prof_bucket *b = &h->buckets[h->index[slot]];
        if (strcmp(b->key, key) == 0) {
            b->count++;
            b->kernel += kernel ? 1 : 0;
            b->weight += weight;
            b->extra += extra;
            return;
        }
        slot = (slot + 1) & (h->index_cap - 1);
    }
    if (h->count == h->cap) {
        size_t cap = h->cap ? h->cap * 2 : 64;
        struct prof_bucket *b = realloc(h->buckets, cap * sizeof *b);
        if (!b)
            return;
        h->buckets = b;
        h->cap = cap;
    }
    struct prof_bucket *b = &h->buckets[h->count];
    b->key = strdup(key);
    b->count = 1;
    b->kernel = kernel ? 1 : 0;
    b->weight = weight;
    b->extra = extra;
    if (b->key) {
        h->index[slot] = (int)h->count;
        h->count++;
    }
}

static int cmp_bucket(const void *a, const void *b)
{
    const struct prof_bucket *x = a, *y = b;
    if (x->weight != y->weight)
        return x->weight < y->weight ? 1 : -1;
    if (x->count != y->count)
        return x->count < y->count ? 1 : -1;
    return strcmp(x->key, y->key);
}

void prof_hist_sort(struct prof_hist *h)
{
    qsort(h->buckets, h->count, sizeof *h->buckets, cmp_bucket);
    h->index_stale = 1;         /* the bucket numbers in the index moved */
}

void prof_format_addr(const struct prof_symtab *user, const struct prof_symtab *kernel, int is_kernel,
                      uint64_t addr, int with_offset, char *buf, size_t size)
{
    uint64_t off = 0;
    const struct prof_sym *s = prof_symtab_lookup(is_kernel ? kernel : user, addr, &off);
    if (!s)
        snprintf(buf, size, "0x%lx", (unsigned long)addr);
    else if (with_offset && off)
        snprintf(buf, size, "%s+0x%lx", s->name, (unsigned long)off);
    else
        snprintf(buf, size, "%s", s->name);
}

int prof_is_lock_primitive(const char *name)
{
    static const char *const names[] = {
        "pop_cli", "push_cli", "spin_lock", "spin_unlock", "spin_lock_irqsave", "spin_unlock_irqrestore",
        "tlb_shootdown_poll", "spin_holding",
    };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strcmp(name, names[i]) == 0)
            return 1;
    return 0;
}

/* ---- resolver ---- */

#define RESOLVER_PROCS 64
#define RESOLVER_CACHE 8192u        /* a power of two, probed linearly */

struct resolver_proc {
    pid_t pid;
    struct prof_symtab *syms;
    char name[32];
    int tried;
};

/* Addresses repeat constantly, so a formatted name is produced once and
 * handed out by pointer afterwards. The table is keyed by the address
 * together with the process it belongs to, since the same user address
 * means different things in different programs. */
struct resolver_entry {
    uint64_t addr;
    pid_t pid;
    int kernel;
    char *name;
};

struct prof_resolver {
    struct prof_symtab *kernel;
    struct resolver_proc procs[RESOLVER_PROCS];
    int nprocs;
    struct resolver_entry *cache;
    size_t cached;
};

struct prof_resolver *prof_resolver_new(void)
{
    return calloc(1, sizeof(struct prof_resolver));
}

void prof_resolver_free(struct prof_resolver *r)
{
    if (!r)
        return;
    prof_symtab_free(r->kernel);
    for (int i = 0; i < r->nprocs; i++)
        prof_symtab_free(r->procs[i].syms);
    if (r->cache) {
        for (size_t i = 0; i < RESOLVER_CACHE; i++)
            free(r->cache[i].name);
        free(r->cache);
    }
    free(r);
}

int prof_resolver_kernel(struct prof_resolver *r)
{
    if (!r->kernel)
        r->kernel = prof_symtab_load_kernel();
    return r->kernel ? 0 : -1;
}

/* The PID PPID PGID STATE TIME RSS UID NAME table of /dev/proc. */
static int read_proc_name(pid_t pid, char *out, size_t size)
{
    FILE *f = fopen("/dev/proc", "r");
    if (!f)
        return -1;
    char line[256];
    int found = -1;
    while (found < 0 && fgets(line, sizeof line, f)) {
        char *end;
        long p = strtol(line, &end, 10);
        if (end == line || p != pid)
            continue;
        char *tok = end;
        for (int col = 0; col < 6; col++) {
            while (*tok == ' ')
                tok++;
            while (*tok && *tok != ' ')
                tok++;
        }
        while (*tok == ' ')
            tok++;
        tok[strcspn(tok, "\n")] = '\0';
        snprintf(out, size, "%s", tok);
        found = 0;
    }
    fclose(f);
    return found;
}

static struct resolver_proc *resolver_proc(struct prof_resolver *r, pid_t pid)
{
    for (int i = 0; i < r->nprocs; i++)
        if (r->procs[i].pid == pid)
            return &r->procs[i];
    if (r->nprocs == RESOLVER_PROCS)
        return NULL;
    struct resolver_proc *p = &r->procs[r->nprocs++];
    p->pid = pid;
    p->tried = 0;
    p->name[0] = '\0';
    p->syms = NULL;
    return p;
}

const char *prof_resolver_procname(struct prof_resolver *r, pid_t pid)
{
    struct resolver_proc *p = resolver_proc(r, pid);
    if (!p)
        return NULL;
    if (!p->name[0])
        read_proc_name(pid, p->name, sizeof p->name);
    return p->name[0] ? p->name : NULL;
}

/* The executable of a process, looked for where programs are installed. */
static struct prof_symtab *resolver_symtab(struct prof_resolver *r, pid_t pid)
{
    struct resolver_proc *p = resolver_proc(r, pid);
    if (!p || p->tried)
        return p ? p->syms : NULL;
    p->tried = 1;
    const char *name = prof_resolver_procname(r, pid);
    if (!name)
        return NULL;
    static const char *const dirs[] = { "/usr/bin", LOCAL_BIN };
    char path[160];
    for (size_t i = 0; i < sizeof dirs / sizeof dirs[0] && !p->syms; i++) {
        snprintf(path, sizeof path, "%s/%s", dirs[i], name);
        p->syms = prof_symtab_load_elf(path);
    }
    prof_symtab_add_maps(p->syms, pid);
    return p->syms;
}

static size_t cache_slot(uint64_t addr, pid_t pid, int kernel)
{
    uint64_t h = addr * 0x9e3779b97f4a7c15ULL;
    h ^= (uint64_t)pid * 0x100000001b3ULL;
    h ^= (uint64_t)kernel << 33;
    return (size_t)(h >> 33) & (RESOLVER_CACHE - 1);
}

const char *prof_resolve(struct prof_resolver *r, pid_t pid, int kernel, uint64_t addr)
{
    if (kernel)
        pid = 0;                /* one kernel, whoever was running */
    if (!r->cache) {
        r->cache = calloc(RESOLVER_CACHE, sizeof *r->cache);
        if (!r->cache)
            return "?";
    }
    size_t slot = cache_slot(addr, pid, kernel);
    for (size_t i = 0; i < RESOLVER_CACHE; i++) {
        struct resolver_entry *e = &r->cache[(slot + i) & (RESOLVER_CACHE - 1)];
        if (!e->name) {
            char buf[160];
            const struct prof_symtab *t = kernel ? r->kernel : resolver_symtab(r, pid);
            prof_format_addr(kernel ? NULL : t, kernel ? t : NULL, kernel, addr, 0, buf, sizeof buf);
            char *copy = strdup(buf);
            if (!copy)
                return "?";
            /* A full table is emptied rather than grown: the working set
             * of a profile is bounded and starting over is cheap. */
            if (r->cached * 4 >= RESOLVER_CACHE * 3) {
                for (size_t k = 0; k < RESOLVER_CACHE; k++) {
                    free(r->cache[k].name);
                    r->cache[k].name = NULL;
                }
                r->cached = 0;
                e = &r->cache[cache_slot(addr, pid, kernel)];
            }
            e->addr = addr;
            e->pid = pid;
            e->kernel = kernel;
            e->name = copy;
            r->cached++;
            return copy;
        }
        if (e->addr == addr && e->pid == pid && e->kernel == kernel)
            return e->name;
    }
    return "?";
}
