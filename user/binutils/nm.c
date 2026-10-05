/* nm: list the symbols of ELF64 objects, shared objects, programs and
 * archives of them, with the output formats of GNU nm (bsd, sysv, posix
 * and just-symbols).  The symbol type letters follow the rules of the
 * BFD library for ELF symbols and sections.
 *
 *     nm [-aABDfgjnoPprSsuUW] [-t radix] [-f format] [--size-sort] [file...]
 */
#include "lib/binutils.h"
#include <errno.h>
#include <getopt.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The classes of the section of a symbol. */
enum { CLS_NORMAL, CLS_UND, CLS_ABS, CLS_COM };

/* The section flags that the BFD library derives from an ELF section. */
#define SF_ALLOC 1
#define SF_LOAD 2
#define SF_CONTENTS 4
#define SF_READONLY 8
#define SF_CODE 16
#define SF_DATA 32
#define SF_DEBUG 64

/* The symbol flags of the BFD library that nm tests. */
#define BF_LOCAL 1
#define BF_GLOBAL 2
#define BF_WEAK 4
#define BF_UNIQUE 8
#define BF_DEBUG 16
#define BF_OBJECT 32
#define BF_IFUNC 64

enum { FMT_BSD, FMT_SYSV, FMT_POSIX, FMT_JUST };

struct nsym {
    const char *name;
    uint64_t value;
    uint64_t size;
    unsigned flags;
    unsigned cls;
    unsigned sflags;
    unsigned stype;
    const char *secname;
    uint64_t secend;        /* end address of the section, for the size sort */
    unsigned shndx;
    char type;
};

static int opt_debug, opt_file, opt_dynamic, opt_extern, opt_numeric, opt_nosort;
static int opt_reverse, opt_size, opt_sizesort, opt_undef, opt_defined, opt_armap;
static int opt_noweak, opt_special, opt_quiet, opt_format = FMT_BSD, opt_radix = 'x';
static int nfiles;

/* Return the flags that the BFD library gives to the section s. */
static unsigned section_flags(const struct elffile *f, const Elf64_Shdr *s)
{
    unsigned fl = 0;
    if (s->sh_type != SHT_NOBITS)
        fl |= SF_CONTENTS;
    if (s->sh_flags & SHF_ALLOC) {
        fl |= SF_ALLOC;
        if (s->sh_type != SHT_NOBITS)
            fl |= SF_LOAD;
    }
    if (!(s->sh_flags & SHF_WRITE))
        fl |= SF_READONLY;
    if (s->sh_flags & SHF_EXECINSTR)
        fl |= SF_CODE;
    else if (fl & SF_LOAD)
        fl |= SF_DATA;
    if (!(fl & SF_ALLOC)) {
        const char *n = elffile_section_name(f, s);
        if (n[0] == '.' && (strncmp(n, ".debug", 6) == 0 || strncmp(n, ".gnu.linkonce.wi.", 17) == 0 ||
                            strncmp(n, ".line", 5) == 0 || strncmp(n, ".stab", 5) == 0 ||
                            strncmp(n, ".zdebug", 7) == 0))
            fl |= SF_DEBUG;
    }
    return fl;
}

/* Return the letter of a section for the symbols it contains. */
static char section_letter(unsigned fl)
{
    if (fl & SF_CODE)
        return 't';
    if (fl & SF_DATA)
        return (fl & SF_READONLY) ? 'r' : 'd';
    if (!(fl & SF_CONTENTS))
        return 'b';
    if (fl & SF_DEBUG)
        return 'N';
    if (fl & SF_READONLY)
        return 'n';
    return '?';
}

/* Return the type letter of a symbol, as bfd_decode_symclass does. */
static char symbol_letter(const struct nsym *s)
{
    if (s->cls == CLS_COM)
        return 'C';
    if (s->cls == CLS_UND) {
        if (s->flags & BF_WEAK)
            return (s->flags & BF_OBJECT) ? 'v' : 'w';
        return 'U';
    }
    if (s->flags & BF_IFUNC)
        return 'i';
    if (s->flags & BF_WEAK)
        return (s->flags & BF_OBJECT) ? 'V' : 'W';
    if (s->flags & BF_UNIQUE)
        return 'u';
    if (!(s->flags & (BF_GLOBAL | BF_LOCAL)))
        return '?';
    char c = s->cls == CLS_ABS ? 'a' : section_letter(s->sflags);
    if (s->flags & BF_GLOBAL)
        c = (char)(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
    return c;
}

/* Report whether the machine of f marks the symbol name as special. */
static int special_name(const struct elffile *f, const char *name)
{
    if (f->eh->e_machine != EM_AARCH64)
        return 0;
    return name[0] == '$' && (name[1] == 'x' || name[1] == 'd') && (name[2] == '\0' || name[2] == '.');
}

/* Fill in the record of the symbol es of the table with the string table
 * strtab. */
static void make_symbol(const struct elffile *f, const Elf64_Shdr *strtab, const Elf64_Sym *es, struct nsym *s)
{
    memset(s, 0, sizeof *s);
    s->name = elffile_symbol_name(f, strtab, es);
    s->value = es->st_value;
    s->size = es->st_size;
    s->shndx = es->st_shndx;
    s->stype = ELF64_ST_TYPE(es->st_info);
    unsigned bind = ELF64_ST_BIND(es->st_info);
    const Elf64_Shdr *sec = NULL;
    if (es->st_shndx == SHN_UNDEF) {
        s->cls = CLS_UND;
        s->secname = "*UND*";
    } else if (es->st_shndx == SHN_COMMON) {
        s->cls = CLS_COM;
        s->secname = "*COM*";
        s->value = es->st_size;
    } else if (es->st_shndx == SHN_ABS || (sec = elffile_section(f, es->st_shndx)) == NULL) {
        s->cls = CLS_ABS;
        s->secname = "*ABS*";
    } else {
        s->cls = CLS_NORMAL;
        s->secname = elffile_section_name(f, sec);
        s->sflags = section_flags(f, sec);
        s->secend = sec->sh_addr + sec->sh_size;
    }
    switch (bind) {
    case STB_LOCAL:
        s->flags |= BF_LOCAL;
        break;
    case STB_GLOBAL:
        if (s->cls != CLS_UND && s->cls != CLS_COM)
            s->flags |= BF_GLOBAL;
        break;
    case STB_WEAK:
        s->flags |= BF_WEAK;
        break;
    case STB_GNU_UNIQUE:
        s->flags |= BF_UNIQUE;
        break;
    default:
        break;
    }
    switch (s->stype) {
    case STT_SECTION:
    case STT_FILE:
        s->flags |= BF_DEBUG;
        break;
    case STT_OBJECT:
        s->flags |= BF_OBJECT;
        break;
    case STT_GNU_IFUNC:
        s->flags |= BF_IFUNC;
        break;
    default:
        break;
    }
    s->type = symbol_letter(s);
}

/* Order the symbols by name.  Symbols with equal names compare equal, so
 * the order among them is the order that qsort gives, as in GNU nm. */
static int cmp_name(const void *a, const void *b)
{
    const struct nsym *x = a, *y = b;
    return strcoll(x->name, y->name);
}

/* Order the symbols by address with the undefined symbols first. */
static int cmp_numeric(const void *a, const void *b)
{
    const struct nsym *x = a, *y = b;
    if (x->cls == CLS_UND) {
        if (y->cls != CLS_UND)
            return -1;
    } else if (y->cls == CLS_UND) {
        return 1;
    } else if (x->value != y->value) {
        return x->value < y->value ? -1 : 1;
    }
    return cmp_name(a, b);
}

static int cmp_size(const void *a, const void *b)
{
    const struct nsym *x = a, *y = b;
    if (x->size != y->size)
        return x->size < y->size ? -1 : 1;
    return cmp_name(a, b);
}

static int cmp_sort(const void *a, const void *b)
{
    int r = opt_sizesort ? cmp_size(a, b) : opt_numeric ? cmp_numeric(a, b) : cmp_name(a, b);
    return opt_reverse ? -r : r;
}

/* Order pointers to symbols by address, section and name. */
static int cmp_address_ptr(const void *a, const void *b)
{
    const struct nsym *x = *(const struct nsym *const *)a, *y = *(const struct nsym *const *)b;
    if (x->value != y->value)
        return x->value < y->value ? -1 : 1;
    if (x->shndx != y->shndx)
        return x->shndx < y->shndx ? -1 : 1;
    return cmp_name(x, y);
}

/* Give each symbol of size zero the distance to the next symbol in the
 * address order when both lie in one section, else the distance to the
 * end of the section.  The symbols that remain without size are dropped.
 * The symbols that remain are in the address order.  The result is the
 * new count. */
static size_t size_sort_prepare(struct nsym *v, size_t n)
{
    struct nsym **p = bu_alloc(n * sizeof *p);
    for (size_t i = 0; i < n; i++)
        p[i] = &v[i];
    qsort(p, n, sizeof *p, cmp_address_ptr);
    for (size_t i = 0; i < n; i++) {
        struct nsym *cur = p[i];
        if (cur->size)
            continue;
        uint64_t end = cur->secend;
        if (i + 1 < n && p[i + 1]->shndx == cur->shndx && p[i + 1]->cls == cur->cls)
            end = p[i + 1]->value;
        cur->size = end - cur->value;
    }
    struct nsym *out = bu_alloc(n * sizeof *out);
    size_t k = 0;
    for (size_t i = 0; i < n; i++)
        if (p[i]->size)
            out[k++] = *p[i];
    memcpy(v, out, k * sizeof *v);
    free(out);
    free(p);
    return k;
}

/* Print a number in the radix, zero filled to the width of 64 bit values. */
static void print_value(uint64_t v)
{
    if (opt_radix == 'd')
        printf("%016llu", (unsigned long long)v);
    else if (opt_radix == 'o')
        printf("%016llo", (unsigned long long)v);
    else
        printf("%016llx", (unsigned long long)v);
}

/* Print a number in the radix without filling. */
static void print_plain(uint64_t v)
{
    if (opt_radix == 'd')
        printf("%llu", (unsigned long long)v);
    else if (opt_radix == 'o')
        printf("%llo", (unsigned long long)v);
    else
        printf("%llx", (unsigned long long)v);
}

/* Print the label that -A puts before each symbol. */
static void print_label(const char *path, const char *member)
{
    if (!opt_file)
        return;
    if (opt_format == FMT_POSIX) {
        if (member)
            printf("%s[%s]: ", path, member);
        else
            printf("%s: ", path);
    } else if (member) {
        printf("%s:%s:", path, member);
    } else {
        printf("%s:", path);
    }
}

static const char *sysv_type_name(unsigned type)
{
    static char buf[40];
    switch (type) {
    case STT_NOTYPE:
        return "NOTYPE";
    case STT_OBJECT:
        return "OBJECT";
    case STT_FUNC:
        return "FUNC";
    case STT_SECTION:
        return "SECTION";
    case STT_FILE:
        return "FILE";
    case STT_COMMON:
        return "COMMON";
    case STT_TLS:
        return "TLS";
    default:
        break;
    }
    if (type >= STT_LOPROC)
        snprintf(buf, sizeof buf, "<processor specific>: %u", type);
    else if (type >= STT_LOOS)
        snprintf(buf, sizeof buf, "<OS specific>: %u", type);
    else
        snprintf(buf, sizeof buf, "<unknown>: %u", type);
    return buf;
}

static void print_symbol(const char *path, const char *member, const struct nsym *s)
{
    int undef = s->cls == CLS_UND;
    switch (opt_format) {
    case FMT_JUST:
        printf("%s\n", s->name);
        break;
    case FMT_POSIX:
        print_label(path, member);
        printf("%s %c ", s->name, s->type);
        if (undef) {
            printf("%8s", "");
        } else {
            print_plain(s->value);
            printf(" ");
            if (s->size)
                print_plain(s->size);
        }
        printf("\n");
        break;
    case FMT_SYSV:
        if (opt_file) {
            if (member)
                printf("%s:%s:", path, member);
            else
                printf("%s:", path);
        }
        printf("%-20s|", s->name);
        if (undef)
            printf("%16s", "");
        else
            print_value(s->value);
        printf("|   %c  |%18s|", s->type, s->stype == STT_SECTION ? "" : sysv_type_name(s->stype));
        if (s->size && !undef)
            print_value(s->size);
        else
            printf("%16s", "");
        printf("|     |%s\n", s->stype == STT_SECTION ? "" : s->secname);
        break;
    default:
        print_label(path, member);
        if (undef)
            printf("%16s", "");
        else
            print_value(opt_sizesort && !opt_size ? s->size : s->value);
        printf(" ");
        if (opt_size && !undef && s->size) {
            print_value(s->size);
            printf(" ");
        }
        printf("%c %s\n", s->type, s->name);
        break;
    }
}

/* Print the header that precedes the symbols of one object. */
static void print_header(const char *path, const char *member)
{
    if (opt_format == FMT_SYSV) {
        printf("\n\n%s from ", opt_undef ? "Undefined symbols" : "Symbols");
        if (member)
            printf("%s[%s]", path, member);
        else
            printf("%s", path);
        printf(":\n\nName                  Value           Class        Type         Size             Line  Section\n\n");
        return;
    }
    if (opt_file || opt_format == FMT_JUST)
        return;
    if (opt_format == FMT_POSIX) {
        if (member)
            printf("%s[%s]:\n", path, member);
        else if (nfiles > 1)
            printf("%s:\n", path);
        return;
    }
    if (member)
        printf("\n%s:\n", member);
    else if (nfiles > 1)
        printf("\n%s:\n", path);
}

/* Report whether the symbol passes the selection options. */
static int selected(const struct elffile *f, const struct nsym *s)
{
    int show;
    if (opt_undef)
        show = s->cls == CLS_UND;
    else if (opt_extern)
        show = (s->flags & (BF_GLOBAL | BF_WEAK | BF_UNIQUE)) || s->cls == CLS_UND || s->cls == CLS_COM;
    else
        show = 1;
    if (show && !opt_debug && (s->flags & BF_DEBUG))
        show = 0;
    if (show && opt_sizesort && (s->cls == CLS_ABS || s->cls == CLS_UND))
        show = 0;
    if (show && opt_defined && s->cls == CLS_UND)
        show = 0;
    if (show && opt_noweak && (s->flags & BF_WEAK))
        show = 0;
    if (show && !opt_special && special_name(f, s->name))
        show = 0;
    return show;
}

static int show_object(const char *path, const char *member, const struct elffile *f, void *arg)
{
    (void)arg;
    const char *label = member ? member : path;
    print_header(path, member);
    const Elf64_Shdr *tab = opt_dynamic ? elffile_find_type(f, SHT_DYNSYM) : elffile_find_type(f, SHT_SYMTAB);
    size_t count = 0;
    const Elf64_Shdr *strtab = NULL;
    const Elf64_Sym *syms = tab ? elffile_symbols(f, tab, &count, &strtab) : NULL;
    if (!syms || count <= 1) {
        if (!opt_quiet)
            bu_error(NULL, "%s: no symbols", label);
        return 0;
    }
    struct nsym *v = bu_alloc(count * sizeof *v);
    size_t n = 0;
    for (size_t i = 1; i < count; i++) {
        struct nsym s;
        make_symbol(f, strtab, &syms[i], &s);
        if (selected(f, &s))
            v[n++] = s;
    }
    if (opt_sizesort)
        n = size_sort_prepare(v, n);
    if (!opt_nosort)
        qsort(v, n, sizeof *v, cmp_sort);
    for (size_t i = 0; i < n; i++)
        print_symbol(path, member, &v[i]);
    free(v);
    return 0;
}

/* Print the symbol index of an archive. */
static int show_archive(const char *path, const struct archive *a, void *arg)
{
    (void)arg;
    if (opt_format == FMT_BSD && nfiles > 1)
        printf("\n%s:\n", path);
    if (!opt_armap)
        return 0;
    size_t n = archive_index_count(a);
    if (n == 0)
        return 0;
    printf("\n");
    printf("Archive index:\n");
    for (size_t i = 0; i < n; i++) {
        const char *name;
        size_t off;
        if (!archive_index_entry(a, i, &name, &off))
            continue;
        const char *member = "";
        for (const struct ar_member *m = a->members; m; m = m->next)
            if (m->offset == off) {
                member = m->name;
                break;
            }
        printf("%s in %s\n", name, member);
    }
    return 0;
}

static const struct option longopts[] = {
    { "debug-syms", no_argument, NULL, 'a' },
    { "print-file-name", no_argument, NULL, 'A' },
    { "dynamic", no_argument, NULL, 'D' },
    { "format", required_argument, NULL, 'f' },
    { "extern-only", no_argument, NULL, 'g' },
    { "just-symbols", no_argument, NULL, 'j' },
    { "numeric-sort", no_argument, NULL, 'n' },
    { "no-sort", no_argument, NULL, 'p' },
    { "portability", no_argument, NULL, 'P' },
    { "reverse-sort", no_argument, NULL, 'r' },
    { "print-size", no_argument, NULL, 'S' },
    { "print-armap", no_argument, NULL, 's' },
    { "radix", required_argument, NULL, 't' },
    { "undefined-only", no_argument, NULL, 'u' },
    { "defined-only", no_argument, NULL, 'U' },
    { "no-weak", no_argument, NULL, 'W' },
    { "size-sort", no_argument, NULL, 256 },
    { "special-syms", no_argument, NULL, 257 },
    { "quiet", no_argument, NULL, 258 },
    { "demangle", no_argument, NULL, 'C' },
    { "no-demangle", no_argument, NULL, 259 },
    { "line-numbers", no_argument, NULL, 'l' },
    { "help", no_argument, NULL, 'h' },
    { NULL, 0, NULL, 0 },
};

static void usage(int status)
{
    fprintf(status ? stderr : stdout, "Usage: nm [-aABDfgjnoPprSsuUW] [-t radix] [-f format] [--size-sort] [file...]\n");
    exit(status);
}

static void set_format(const char *s)
{
    if (strcmp(s, "bsd") == 0)
        opt_format = FMT_BSD;
    else if (strcmp(s, "sysv") == 0)
        opt_format = FMT_SYSV;
    else if (strcmp(s, "posix") == 0)
        opt_format = FMT_POSIX;
    else if (strcmp(s, "just-symbols") == 0)
        opt_format = FMT_JUST;
    else
        bu_fatal(NULL, "invalid output format");
}

static void set_radix(const char *s)
{
    if (s[0] && !s[1] && (s[0] == 'd' || s[0] == 'o' || s[0] == 'x'))
        opt_radix = s[0];
    else
        bu_fatal(NULL, "%s: invalid radix", s);
}

static void apply(int code, const char *arg)
{
    switch (code) {
    case 'a': opt_debug = 1; break;
    case 'A': case 'o': opt_file = 1; break;
    case 'B': opt_format = FMT_BSD; break;
    case 'D': opt_dynamic = 1; break;
    case 'f': set_format(arg); break;
    case 'g': opt_extern = 1; break;
    case 'j': opt_format = FMT_JUST; break;
    case 'n': case 'v': opt_numeric = 1; break;
    case 'p': opt_nosort = 1; break;
    case 'P': opt_format = FMT_POSIX; break;
    case 'r': opt_reverse = 1; break;
    case 'S': opt_size = 1; break;
    case 's': opt_armap = 1; break;
    case 't': set_radix(arg); break;
    case 'u': opt_undef = 1; opt_defined = 0; break;
    case 'U': opt_defined = 1; opt_undef = 0; break;
    case 'W': opt_noweak = 1; break;
    case 256: opt_sizesort = 1; break;
    case 257: opt_special = 1; break;
    case 258: opt_quiet = 1; break;
    case 'C': case 'e': case 'l': case 'X': case 259: break;     /* accepted without effect */
    case 'h': usage(0); break;
    default: usage(1); break;
    }
}

int main(int argc, char **argv)
{
    bu_program = "nm";
    setlocale(LC_ALL, "");
    int c;
    while ((c = getopt_long(argc, argv, "aABDf:gjnopPrSst:uUvWCelX:h", longopts, NULL)) != -1)
        apply(c, optarg);
    static char *default_file[] = { "a.out" };
    char **files = argv + optind;
    nfiles = argc - optind;
    if (nfiles == 0) {
        files = default_file;
        nfiles = 1;
    }
    int status = 0;
    for (int i = 0; i < nfiles; i++)
        if (bu_for_each_object(files[i], show_archive, show_object, NULL, NULL))
            status = 1;
    fflush(stdout);
    return status;
}
