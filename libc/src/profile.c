#include <minios/profile.h>
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

ssize_t prof_read(int fd, struct prof_sample *buf, size_t max)
{
    ssize_t n = read(fd, buf, max * sizeof *buf);
    return n < 0 ? n : n / (ssize_t)sizeof *buf;
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
    free(t->syms);
    free(t->strings);
    free(t);
}

const struct prof_sym *prof_symtab_lookup(const struct prof_symtab *t, uint64_t addr, uint64_t *off)
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
    memset(h, 0, sizeof *h);
}

void prof_hist_add(struct prof_hist *h, const char *key, int kernel)
{
    h->total++;
    for (size_t i = 0; i < h->count; i++) {
        if (strcmp(h->buckets[i].key, key) == 0) {
            h->buckets[i].count++;
            h->buckets[i].kernel += kernel ? 1 : 0;
            return;
        }
    }
    if (h->count == h->cap) {
        size_t cap = h->cap ? h->cap * 2 : 64;
        struct prof_bucket *b = realloc(h->buckets, cap * sizeof *b);
        if (!b)
            return;
        h->buckets = b;
        h->cap = cap;
    }
    h->buckets[h->count].key = strdup(key);
    h->buckets[h->count].count = 1;
    h->buckets[h->count].kernel = kernel ? 1 : 0;
    if (h->buckets[h->count].key)
        h->count++;
}

static int cmp_bucket(const void *a, const void *b)
{
    const struct prof_bucket *x = a, *y = b;
    if (x->count != y->count)
        return x->count < y->count ? 1 : -1;
    return strcmp(x->key, y->key);
}

void prof_hist_sort(struct prof_hist *h)
{
    qsort(h->buckets, h->count, sizeof *h->buckets, cmp_bucket);
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
