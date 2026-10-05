/* The parts of an ELF64 file the library rule reads: the DT_NEEDED
 * entries and the dynamic symbol table, through the section headers,
 * which the installed files retain (objcopy --strip-debug).  The reader
 * of <minios/elffile.h> checks every offset and size. */
#include "pkg.h"
#include <minios/elffile.h>
#include <string.h>

int elf_is(const uint8_t *data, size_t len)
{
    return len >= sizeof(Elf64_Ehdr) && memcmp(data, ELFMAG, SELFMAG) == 0 && data[EI_CLASS] == ELFCLASS64 &&
           data[EI_DATA] == ELFDATA2LSB;
}

const char *elf_arch(const uint8_t *data, size_t len)
{
    if (!elf_is(data, len))
        return "unknown";
    switch (((const Elf64_Ehdr *)data)->e_machine) {
    case EM_X86_64:  return "x86_64";
    case EM_AARCH64: return "aarch64";
    default:         return "unknown";
    }
}

/* The DT_NEEDED names, at most max; the count, or -1 when the file is not
 * a dynamic ELF file. */
int elf_needed(const uint8_t *data, size_t len, char (*names)[PKG_NAME_MAX], int max)
{
    struct elffile f;
    if (elffile_open(&f, data, len) < 0 || !elffile_find_type(&f, SHT_DYNAMIC))
        return -1;
    size_t count;
    const Elf64_Shdr *str;
    const Elf64_Dyn *d = elffile_dynamic(&f, &count, &str);
    if (!d || !str)
        return -1;
    int found = 0;
    for (size_t j = 0; j < count; j++) {
        if (d[j].d_tag != DT_NEEDED)
            continue;
        const char *s = elffile_string(&f, str, d[j].d_un.d_val);
        if (!s || strlen(s) >= PKG_NAME_MAX)
            return -1;
        if (found < max)
            strlcpy(names[found], s, PKG_NAME_MAX);
        found++;
    }
    return found;
}

/* dynsym returns the dynamic symbols of the file, or NULL. */
static const Elf64_Sym *dynsym(struct elffile *f, const uint8_t *data, size_t len, size_t *count,
                               const Elf64_Shdr **strtab)
{
    if (elffile_open(f, data, len) < 0)
        return NULL;
    return elffile_symbols(f, elffile_find_type(f, SHT_DYNSYM), count, strtab);
}

/* fn for every undefined symbol the loader must resolve (weak references
 * may remain unresolved). Returns 0, fn's first non-zero result, or -1 for a
 * file without a dynamic symbol table. */
int elf_undefined(const uint8_t *data, size_t len, elf_symbol_fn fn, void *arg)
{
    struct elffile f;
    size_t count;
    const Elf64_Shdr *str;
    const Elf64_Sym *s = dynsym(&f, data, len, &count, &str);
    if (!s)
        return -1;
    for (size_t i = 1; i < count; i++) {
        if (s[i].st_shndx != SHN_UNDEF || ELF64_ST_BIND(s[i].st_info) == STB_WEAK)
            continue;
        const char *name = elffile_string(&f, str, s[i].st_name);
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
    struct elffile f;
    size_t count;
    const Elf64_Shdr *str;
    const Elf64_Sym *s = dynsym(&f, data, len, &count, &str);
    for (size_t i = 1; s && i < count; i++) {
        if (s[i].st_shndx == SHN_UNDEF)
            continue;
        const char *n = elffile_string(&f, str, s[i].st_name);
        if (n && strcmp(n, name) == 0)
            return 1;
    }
    return 0;
}

/* Calls fn for every symbol the dynamic symbol table defines. */
int elf_defined(const uint8_t *data, size_t len, elf_symbol_fn fn, void *arg)
{
    struct elffile f;
    size_t count;
    const Elf64_Shdr *str;
    const Elf64_Sym *s = dynsym(&f, data, len, &count, &str);
    for (size_t i = 1; s && i < count; i++) {
        if (s[i].st_shndx == SHN_UNDEF)
            continue;
        const char *n = elffile_string(&f, str, s[i].st_name);
        if (n && fn(n, arg) != 0)
            return 1;
    }
    return 0;
}
