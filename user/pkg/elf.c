/* The parts of an ELF64 file the library rule reads: the DT_NEEDED
 * entries and the dynamic symbol table, through the section headers,
 * which the installed files retain (objcopy --strip-debug). */
#include "pkg.h"
#include <string.h>

struct ehdr {
    unsigned char e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};
struct shdr {
    uint32_t sh_name, sh_type;
    uint64_t sh_flags, sh_addr, sh_offset, sh_size;
    uint32_t sh_link, sh_info;
    uint64_t sh_addralign, sh_entsize;
};
struct sym {
    uint32_t st_name;
    unsigned char st_info, st_other;
    uint16_t st_shndx;
    uint64_t st_value, st_size;
};
struct dyn { int64_t d_tag; uint64_t d_val; };

#define SHT_STRTAB 3
#define SHT_DYNAMIC 6
#define SHT_DYNSYM 11
#define DT_NULL 0
#define DT_NEEDED 1
#define STB_WEAK 2
#define SHN_UNDEF 0

int elf_is(const uint8_t *data, size_t len)
{
    return len >= sizeof(struct ehdr) && memcmp(data, "\x7f" "ELF", 4) == 0 && data[4] == 2 && data[5] == 1;
}

#define EM_X86_64 62
#define EM_AARCH64 183

const char *elf_arch(const uint8_t *data, size_t len)
{
    if (!elf_is(data, len))
        return "unknown";
    switch (((const struct ehdr *)data)->e_machine) {
    case EM_X86_64:  return "x86_64";
    case EM_AARCH64: return "aarch64";
    default:         return "unknown";
    }
}

static const struct shdr *sections(const uint8_t *data, size_t len, int *count)
{
    if (!elf_is(data, len))
        return NULL;
    const struct ehdr *eh = (const struct ehdr *)data;
    if (eh->e_shentsize != sizeof(struct shdr) || !eh->e_shnum ||
        eh->e_shoff > len || (size_t)eh->e_shnum * sizeof(struct shdr) > len - eh->e_shoff)
        return NULL;
    *count = eh->e_shnum;
    return (const struct shdr *)(data + eh->e_shoff);
}

static int section_ok(const struct shdr *s, size_t len)
{
    return s->sh_offset <= len && s->sh_size <= len - s->sh_offset;
}

static const char *string_at(const uint8_t *data, const struct shdr *strtab, uint64_t off)
{
    if (off >= strtab->sh_size)
        return NULL;
    const char *s = (const char *)data + strtab->sh_offset + off;
    if (!memchr(s, '\0', strtab->sh_size - off))
        return NULL;
    return s;
}

/* The DT_NEEDED names, at most max; the count, or -1 when the file is not
 * a dynamic ELF file. */
int elf_needed(const uint8_t *data, size_t len, char (*names)[PKG_NAME_MAX], int max)
{
    int n;
    const struct shdr *sh = sections(data, len, &n);
    if (!sh)
        return -1;
    for (int i = 0; i < n; i++) {
        if (sh[i].sh_type != SHT_DYNAMIC || !section_ok(&sh[i], len) || sh[i].sh_link >= (uint32_t)n)
            continue;
        const struct shdr *str = &sh[sh[i].sh_link];
        if (str->sh_type != SHT_STRTAB || !section_ok(str, len))
            return -1;
        const struct dyn *d = (const struct dyn *)(data + sh[i].sh_offset);
        size_t count = sh[i].sh_size / sizeof *d;
        int found = 0;
        for (size_t j = 0; j < count && d[j].d_tag != DT_NULL; j++) {
            if (d[j].d_tag != DT_NEEDED)
                continue;
            const char *s = string_at(data, str, d[j].d_val);
            if (!s || strlen(s) >= PKG_NAME_MAX)
                return -1;
            if (found < max)
                strlcpy(names[found], s, PKG_NAME_MAX);
            found++;
        }
        return found;
    }
    return -1;
}

static const struct shdr *dynsym(const uint8_t *data, size_t len, const struct shdr **strtab)
{
    int n;
    const struct shdr *sh = sections(data, len, &n);
    if (!sh)
        return NULL;
    for (int i = 0; i < n; i++) {
        if (sh[i].sh_type != SHT_DYNSYM || !section_ok(&sh[i], len) || sh[i].sh_link >= (uint32_t)n)
            continue;
        *strtab = &sh[sh[i].sh_link];
        if ((*strtab)->sh_type != SHT_STRTAB || !section_ok(*strtab, len))
            return NULL;
        return &sh[i];
    }
    return NULL;
}

/* fn for every undefined symbol the loader must resolve (weak references
 * may remain unresolved). Returns 0, fn's first non-zero result, or -1 for a
 * file without a dynamic symbol table. */
int elf_undefined(const uint8_t *data, size_t len, elf_symbol_fn fn, void *arg)
{
    const struct shdr *str;
    const struct shdr *ds = dynsym(data, len, &str);
    if (!ds)
        return -1;
    const struct sym *s = (const struct sym *)(data + ds->sh_offset);
    size_t count = ds->sh_size / sizeof *s;
    for (size_t i = 1; i < count; i++) {
        if (s[i].st_shndx != SHN_UNDEF || (s[i].st_info >> 4) == STB_WEAK)
            continue;
        const char *name = string_at(data, str, s[i].st_name);
        if (!name || !*name)
            continue;
        int r = fn(name, arg);
        if (r)
            return r;
    }
    return 0;
}

int elf_defines(const uint8_t *data, size_t len, const char *name)
{
    const struct shdr *str;
    const struct shdr *ds = dynsym(data, len, &str);
    if (!ds)
        return 0;
    const struct sym *s = (const struct sym *)(data + ds->sh_offset);
    size_t count = ds->sh_size / sizeof *s;
    for (size_t i = 1; i < count; i++) {
        if (s[i].st_shndx == SHN_UNDEF)
            continue;
        const char *n = string_at(data, str, s[i].st_name);
        if (n && strcmp(n, name) == 0)
            return 1;
    }
    return 0;
}

/* Calls fn for every symbol the dynamic symbol table defines. */
int elf_defined(const uint8_t *data, size_t len, elf_symbol_fn fn, void *arg)
{
    const struct shdr *str;
    const struct shdr *ds = dynsym(data, len, &str);
    if (!ds)
        return 0;
    const struct sym *s = (const struct sym *)(data + ds->sh_offset);
    size_t count = ds->sh_size / sizeof *s;
    for (size_t i = 1; i < count; i++) {
        if (s[i].st_shndx == SHN_UNDEF)
            continue;
        const char *n = string_at(data, str, s[i].st_name);
        if (n && fn(n, arg) != 0)
            return 1;
    }
    return 0;
}
