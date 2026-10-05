/* size: print the sizes of the sections of ELF64 objects, shared objects,
 * programs and archives of them, in the formats of GNU size (berkeley,
 * sysv and gnu).
 *
 *     size [-ABGdotxf] [--format=format] [--radix=radix] [--common] [file...]
 */
#include "lib/binutils.h"
#include <getopt.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum { FMT_BERKELEY, FMT_SYSV, FMT_GNU };

static int opt_format = FMT_BERKELEY, opt_radix = 10, opt_totals, opt_common;
static int header_done;
static uint64_t total_text, total_data, total_bss;

/* The section flags that the BFD library derives from an ELF section. */
#define SF_ALLOC 1
#define SF_CONTENTS 2
#define SF_READONLY 4
#define SF_CODE 8

static unsigned section_flags(const Elf64_Shdr *s)
{
    unsigned fl = 0;
    if (s->sh_type != SHT_NOBITS)
        fl |= SF_CONTENTS;
    if (s->sh_flags & SHF_ALLOC)
        fl |= SF_ALLOC;
    if (!(s->sh_flags & SHF_WRITE))
        fl |= SF_READONLY;
    if (s->sh_flags & SHF_EXECINSTR)
        fl |= SF_CODE;
    return fl;
}

/* Report whether the BFD library lists the section at index i as a
 * section.  Relocation sections of the symbol table, the symbol tables
 * and the string tables of the symbol table and of the section names are
 * not sections. */
static int listed(const struct elffile *f, unsigned i)
{
    const Elf64_Shdr *s = &f->sh[i];
    const Elf64_Shdr *symtab = elffile_find_type(f, SHT_SYMTAB);
    unsigned symtab_index = symtab ? elffile_section_index(f, symtab) : 0;
    switch (s->sh_type) {
    case SHT_NULL:
    case SHT_SYMTAB:
    case SHT_SYMTAB_SHNDX:
        return 0;
    case SHT_STRTAB:
        if (i == f->eh->e_shstrndx)
            return 0;
        return !(symtab && symtab->sh_link == i);
    case SHT_REL:
    case SHT_RELA:
        return !(symtab_index != 0 && s->sh_link == symtab_index && s->sh_info != 0 && s->sh_info < f->nsections);
    default:
        return 1;
    }
}

/* Format a number in the radix: decimal, octal with a leading zero or
 * hexadecimal with the prefix 0x. */
static const char *number(uint64_t v)
{
    static char buf[4][40];
    static int slot;
    char *b = buf[slot++ & 3];
    if (opt_radix == 8)
        snprintf(b, 40, "0%llo", (unsigned long long)v);
    else if (opt_radix == 16)
        snprintf(b, 40, "0x%llx", (unsigned long long)v);
    else
        snprintf(b, 40, "%llu", (unsigned long long)v);
    return b;
}

/* Sum the sizes of the common symbols of f. */
static uint64_t common_size(const struct elffile *f)
{
    const Elf64_Shdr *tab = elffile_find_type(f, SHT_SYMTAB);
    size_t count = 0;
    const Elf64_Shdr *strtab;
    const Elf64_Sym *syms = tab ? elffile_symbols(f, tab, &count, &strtab) : NULL;
    uint64_t sum = 0;
    for (size_t i = 1; syms && i < count; i++)
        if (syms[i].st_shndx == SHN_COMMON)
            sum += syms[i].st_size;
    return sum;
}

/* Print the label of an object: the file or the member and its archive. */
static void print_label(const char *path, const char *member)
{
    if (member)
        printf("%s (ex %s)", member, path);
    else
        printf("%s", path);
}

static void print_sysv(const char *path, const char *member, const struct elffile *f)
{
    size_t namelen = strlen("section");
    uint64_t total = 0, maxvma = 0, common = opt_common ? common_size(f) : 0;
    for (unsigned i = 1; i < f->nsections; i++) {
        if (!listed(f, i))
            continue;
        size_t n = strlen(elffile_section_name(f, &f->sh[i]));
        if (n > namelen)
            namelen = n;
        total += f->sh[i].sh_size;
        if (f->sh[i].sh_addr > maxvma)
            maxvma = f->sh[i].sh_addr;
    }
    total += common;
    int sizew = (int)strlen(number(total));
    int vmaw = (int)strlen(number(maxvma));
    if (sizew < 4)
        sizew = 4;
    if (vmaw < 4)
        vmaw = 4;
    if (member)
        printf("%s   (ex %s):\n", member, path);
    else
        printf("%s  :\n", path);
    printf("%-*s   %*s   %*s\n", (int)namelen, "section", sizew, "size", vmaw, "addr");
    for (unsigned i = 1; i < f->nsections; i++) {
        if (!listed(f, i))
            continue;
        const Elf64_Shdr *s = &f->sh[i];
        printf("%-*s   %*s   %*s\n", (int)namelen, elffile_section_name(f, s), sizew, number(s->sh_size), vmaw,
               number(s->sh_addr));
    }
    if (opt_common)
        printf("%-*s   %*s   %*s\n", (int)namelen, "*COM*", sizew, number(common), vmaw, number(0));
    printf("%-*s   %*s\n\n\n", (int)namelen, "Total", sizew, number(total));
}

/* Right align the number in the width. */
static void print_number(int width, uint64_t v)
{
    printf("%*s", width, number(v));
}

static void print_sums(const char *path, const char *member, uint64_t text, uint64_t data, uint64_t bss)
{
    uint64_t total = text + data + bss;
    if (opt_format == FMT_GNU) {
        if (!header_done)
            printf("%10s %10s %10s %10s %s\n", "text", "data", "bss", "total", "filename");
        header_done = 1;
        print_number(10, text);
        printf(" ");
        print_number(10, data);
        printf(" ");
        print_number(10, bss);
        printf(" ");
        print_number(10, total);
        printf(" ");
        print_label(path, member);
        printf("\n");
        return;
    }
    if (!header_done)
        printf("   text\t   data\t    bss\t    %s\t    hex\tfilename\n", opt_radix == 8 ? "oct" : "dec");
    header_done = 1;
    print_number(7, text);
    printf("\t");
    print_number(7, data);
    printf("\t");
    print_number(7, bss);
    printf("\t");
    if (opt_radix == 8)
        printf("%7llo", (unsigned long long)total);
    else
        printf("%7llu", (unsigned long long)total);
    printf("\t%7llx\t", (unsigned long long)total);
    print_label(path, member);
    printf("\n");
}

static void sum_sections(const struct elffile *f, uint64_t *text, uint64_t *data, uint64_t *bss)
{
    *text = *data = *bss = 0;
    for (unsigned i = 1; i < f->nsections; i++) {
        const Elf64_Shdr *s = &f->sh[i];
        unsigned fl = section_flags(s);
        if (!(fl & SF_ALLOC) || !listed(f, i))
            continue;
        if (opt_format == FMT_GNU) {
            if (fl & SF_CODE)
                *text += s->sh_size;
            else if (fl & SF_CONTENTS)
                *data += s->sh_size;
            else
                *bss += s->sh_size;
        } else if (fl & (SF_CODE | SF_READONLY)) {
            *text += s->sh_size;
        } else if (fl & SF_CONTENTS) {
            *data += s->sh_size;
        } else {
            *bss += s->sh_size;
        }
    }
    if (opt_common)
        *bss += common_size(f);
}

static int show_object(const char *path, const char *member, const struct elffile *f, void *arg)
{
    (void)arg;
    if (opt_format == FMT_SYSV) {
        print_sysv(path, member, f);
        return 0;
    }
    uint64_t text, data, bss;
    sum_sections(f, &text, &data, &bss);
    total_text += text;
    total_data += data;
    total_bss += bss;
    print_sums(path, member, text, data, bss);
    return 0;
}

static const struct option longopts[] = {
    { "format", required_argument, NULL, 'F' },
    { "radix", required_argument, NULL, 'R' },
    { "totals", no_argument, NULL, 't' },
    { "common", no_argument, NULL, 'c' },
    { "target", required_argument, NULL, 'T' },
    { "help", no_argument, NULL, 'h' },
    { "version", no_argument, NULL, 'V' },
    { NULL, 0, NULL, 0 },
};

static void usage(int status)
{
    fprintf(status ? stderr : stdout, "Usage: size [-ABGdotxf] [--format=format] [--radix=radix] [--common] [file...]\n");
    exit(status);
}

static void set_format(const char *s)
{
    if (strcmp(s, "sysv") == 0)
        opt_format = FMT_SYSV;
    else if (strcmp(s, "berkeley") == 0)
        opt_format = FMT_BERKELEY;
    else if (strcmp(s, "gnu") == 0)
        opt_format = FMT_GNU;
    else
        bu_fatal(NULL, "invalid argument to --format: %s", s);
}

static void set_radix(const char *s)
{
    char *end;
    long r = strtol(s, &end, 10);
    if (*s == '\0' || *end != '\0' || (r != 8 && r != 10 && r != 16))
        bu_fatal(NULL, "Invalid radix: %s", s);
    opt_radix = (int)r;
}

static void apply(int code, const char *arg)
{
    switch (code) {
    case 'A': opt_format = FMT_SYSV; break;
    case 'B': opt_format = FMT_BERKELEY; break;
    case 'G': opt_format = FMT_GNU; break;
    case 'd': opt_radix = 10; break;
    case 'o': opt_radix = 8; break;
    case 'x': opt_radix = 16; break;
    case 't': opt_totals = 1; break;
    case 'F': set_format(arg); break;
    case 'R': set_radix(arg); break;
    case 'c': opt_common = 1; break;
    case 'f': case 'T': break;
    case 'V': case 'v': printf("size (minios binutils)\n"); exit(0);
    case 'h': case 'H': usage(0); break;
    default: usage(1); break;
    }
}

int main(int argc, char **argv)
{
    bu_program = "size";
    int c;
    while ((c = getopt_long(argc, argv, "ABGdoxtfhHvV", longopts, NULL)) != -1)
        apply(c, optarg);
    static char *default_file[] = { "a.out" };
    char **files = argv + optind;
    int nfiles = argc - optind;
    if (nfiles == 0) {
        files = default_file;
        nfiles = 1;
    }
    int status = 0;
    for (int i = 0; i < nfiles; i++)
        if (bu_for_each_object(files[i], NULL, show_object, NULL, NULL))
            status = 1;
    if (opt_totals && opt_format != FMT_SYSV && header_done)
        print_sums("(TOTALS)", NULL, total_text, total_data, total_bss);
    fflush(stdout);
    return status;
}
