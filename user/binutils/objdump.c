/* objdump: display information from ELF64 files in the format of the GNU
 * binutils, without disassembly.  The output equals the output of GNU
 * objdump for the options that the program implements.
 *
 *     objdump [-afhpxtTrRsw] [-j section] file...
 *
 * The program reads executables, shared objects, relocatable objects and
 * the objects in archives of x86_64 and aarch64.
 */
#include "elfdump.h"
#include <getopt.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct options {
    int archive_headers, file_headers, section_headers, private_headers, syms, dyn_syms, relocs, dyn_relocs;
    int contents, wide;
};

static struct options opt;
static const char **only;       /* the section names of the -j options */
static size_t nonly;
static int *only_seen;
static const struct archive *current_archive;

static unsigned long long u64(uint64_t v)
{
    return (unsigned long long)v;
}

/* ---- sections as the BFD library presents them ---- */

/* The BFD section flags that objdump prints. */
enum {
    SEC_HAS_CONTENTS = 1u << 0,
    SEC_ALLOC = 1u << 1,
    SEC_LOAD = 1u << 2,
    SEC_RELOC = 1u << 3,
    SEC_READONLY = 1u << 4,
    SEC_CODE = 1u << 5,
    SEC_DATA = 1u << 6,
    SEC_DEBUGGING = 1u << 7,
    SEC_EXCLUDE = 1u << 8,
    SEC_LINK_ONCE = 1u << 9,
    SEC_THREAD_LOCAL = 1u << 10,
    SEC_GROUP = 1u << 11,
    SEC_OCTETS = 1u << 12,
};

struct bfd_section {
    unsigned elf_index;         /* the index in the section header table */
    const Elf64_Shdr *sh;
    unsigned flags;
    uint64_t lma;
};

/* The sections that objdump lists, in the order of the section header table. */
struct bfd_sections {
    struct bfd_section *list;
    size_t count;
    const Elf64_Shdr *symtab;   /* the main symbol table or NULL */
};

static int is_main_strtab(const struct elffile *f, const Elf64_Shdr *symtab, unsigned index)
{
    if (f->eh->e_shstrndx == index)
        return 1;
    return symtab && symtab->sh_link == index;
}

/* is_reloc_for_section reports whether sh is a relocation section of the section that sh_info selects.
 * BFD lists such a section as the RELOC flag of the target section and not as a section of its own. */
static int is_reloc_for_section(const struct elffile *f, const Elf64_Shdr *symtab, const Elf64_Shdr *sh)
{
    if (sh->sh_type != SHT_REL && sh->sh_type != SHT_RELA)
        return 0;
    if (!symtab || sh->sh_link != elffile_section_index(f, symtab))
        return 0;
    return sh->sh_info != 0 && sh->sh_info < f->nsections;
}

static unsigned bfd_section_flags(const struct elffile *f, const Elf64_Shdr *symtab, unsigned index)
{
    const Elf64_Shdr *s = &f->sh[index];
    unsigned flags = 0;
    if (s->sh_type != SHT_NOBITS)
        flags |= SEC_HAS_CONTENTS;
    if (s->sh_type == SHT_GROUP) {
        flags |= SEC_GROUP;
        const uint32_t *word = elffile_section_data(f, s);
        if (word && s->sh_size >= 4 && (word[0] & 1))
            flags |= SEC_LINK_ONCE;
    }
    if (s->sh_flags & SHF_ALLOC) {
        flags |= SEC_ALLOC;
        if (s->sh_type != SHT_NOBITS)
            flags |= SEC_LOAD;
    }
    if (!(s->sh_flags & SHF_WRITE))
        flags |= SEC_READONLY;
    if (s->sh_flags & SHF_EXECINSTR)
        flags |= SEC_CODE;
    else if (flags & SEC_LOAD)
        flags |= SEC_DATA;
    if (s->sh_flags & SHF_TLS)
        flags |= SEC_THREAD_LOCAL;
    if (s->sh_flags & SHF_EXCLUDE)
        flags |= SEC_EXCLUDE;
    const char *name = elffile_section_name(f, s);
    if (strncmp(name, ".debug", 6) == 0 || strncmp(name, ".gnu.linkonce.wi.", 17) == 0 ||
        strncmp(name, ".line", 5) == 0 || strncmp(name, ".stab", 5) == 0 || strcmp(name, ".gdb_index") == 0 ||
        strncmp(name, ".zdebug", 7) == 0)
        flags |= SEC_DEBUGGING | SEC_OCTETS;
    for (unsigned i = 0; i < f->nsections; i++) {
        const Elf64_Shdr *r = &f->sh[i];
        if (is_reloc_for_section(f, symtab, r) && r->sh_info == index && r->sh_size != 0)
            flags |= SEC_RELOC;
    }
    return flags;
}

static void bfd_sections_load(const struct elffile *f, struct bfd_sections *b)
{
    memset(b, 0, sizeof *b);
    for (unsigned i = 0; i < f->nsections; i++)
        if (f->sh[i].sh_type == SHT_SYMTAB && !b->symtab)
            b->symtab = &f->sh[i];
    b->list = bu_alloc((f->nsections + 1) * sizeof *b->list);
    for (unsigned i = 1; i < f->nsections; i++) {
        const Elf64_Shdr *s = &f->sh[i];
        if (s->sh_type == SHT_NULL || s->sh_type == SHT_SYMTAB || s->sh_type == SHT_SYMTAB_SHNDX)
            continue;
        if (s->sh_type == SHT_STRTAB && is_main_strtab(f, b->symtab, i))
            continue;
        if (is_reloc_for_section(f, b->symtab, s))
            continue;
        struct bfd_section *sec = &b->list[b->count++];
        sec->elf_index = i;
        sec->sh = s;
        sec->flags = bfd_section_flags(f, b->symtab, i);
        sec->lma = s->sh_addr;
        if (s->sh_flags & SHF_ALLOC) {
            for (unsigned k = 0; k < f->nsegments; k++) {
                const Elf64_Phdr *p = &f->ph[k];
                if (p->p_type == PT_LOAD && s->sh_addr >= p->p_vaddr && s->sh_addr - p->p_vaddr < p->p_memsz) {
                    sec->lma = s->sh_addr - p->p_vaddr + p->p_paddr;
                    break;
                }
            }
        }
    }
}

/* section_selected reports whether the -j options select the section. */
static int section_selected(const struct elffile *f, const struct bfd_section *sec)
{
    if (nonly == 0)
        return 1;
    const char *name = elffile_section_name(f, sec->sh);
    int hit = 0;
    for (size_t i = 0; i < nonly; i++)
        if (strcmp(only[i], name) == 0) {
            only_seen[i] = 1;
            hit = 1;
        }
    return hit;
}

static unsigned section_alignment_power(uint64_t align)
{
    unsigned n = 0;
    while (n < 63 && ((uint64_t)1 << n) < align)
        n++;
    return n;
}

static void print_section_flags(unsigned flags)
{
    static const struct {
        unsigned bit;
        const char *name;
    } names[] = {
        { SEC_HAS_CONTENTS, "CONTENTS" }, { SEC_ALLOC, "ALLOC" }, { SEC_LOAD, "LOAD" }, { SEC_RELOC, "RELOC" },
        { SEC_READONLY, "READONLY" }, { SEC_CODE, "CODE" }, { SEC_DATA, "DATA" }, { SEC_DEBUGGING, "DEBUGGING" },
        { SEC_EXCLUDE, "EXCLUDE" }, { SEC_GROUP, "GROUP" }, { SEC_LINK_ONCE, "LINK_ONCE_DISCARD" },
        { SEC_THREAD_LOCAL, "THREAD_LOCAL" }, { SEC_OCTETS, "OCTETS" },
    };
    int first = 1;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (flags & names[i].bit) {
            printf("%s%s", first ? "" : ", ", names[i].name);
            first = 0;
        }
}

static void dump_section_headers(const struct elffile *f, const struct bfd_sections *b)
{
    printf("Sections:\n");
    size_t width = 13;
    if (opt.wide)
        for (size_t i = 0; i < b->count; i++) {
            size_t len = strlen(elffile_section_name(f, b->list[i].sh));
            if (len > width && section_selected(f, &b->list[i]))
                width = len;
        }
    printf("Idx %-*s Size      VMA               LMA               File off  Algn", (int)width, "Name");
    if (opt.wide)
        printf("  Flags");
    printf("\n");
    for (size_t i = 0; i < b->count; i++) {
        const struct bfd_section *sec = &b->list[i];
        if (!section_selected(f, sec))
            continue;
        const Elf64_Shdr *s = sec->sh;
        printf("%3zu %-*s %08llx  %016llx  %016llx  %08llx  2**%u", i, (int)width, elffile_section_name(f, s),
               u64(s->sh_size), u64(s->sh_addr), u64(sec->lma), u64(s->sh_offset), section_alignment_power(s->sh_addralign));
        if (opt.wide)
            printf("  ");
        else
            printf("\n                  ");
        print_section_flags(sec->flags);
        printf("\n");
    }
}

/* ---- file header ---- */

static const char *architecture_name(unsigned machine)
{
    switch (machine) {
    case EM_X86_64:  return "i386:x86-64";
    case EM_AARCH64: return "aarch64";
    default:         return "UNKNOWN!";
    }
}

static unsigned bfd_file_flags(const struct elffile *f, const struct bfd_sections *b)
{
    unsigned flags = 0;
    for (size_t i = 0; i < b->count; i++)
        if (b->list[i].flags & SEC_RELOC)
            flags |= 0x01;
    if (f->eh->e_type == ET_EXEC)
        flags |= 0x02;
    if (b->symtab && b->symtab->sh_size / sizeof(Elf64_Sym) > 1)
        flags |= 0x10;
    if (f->eh->e_type == ET_DYN)
        flags |= 0x40;
    if (f->nsegments > 0)
        flags |= 0x100;
    return flags;
}

static void dump_file_header(const struct elffile *f, const struct bfd_sections *b)
{
    static const struct {
        unsigned bit;
        const char *name;
    } names[] = {
        { 0x01, "HAS_RELOC" }, { 0x02, "EXEC_P" }, { 0x04, "HAS_LINENO" }, { 0x08, "HAS_DEBUG" },
        { 0x10, "HAS_SYMS" }, { 0x20, "HAS_LOCALS" }, { 0x40, "DYNAMIC" }, { 0x80, "WP_TEXT" },
        { 0x100, "D_PAGED" },
    };
    unsigned flags = bfd_file_flags(f, b);
    printf("architecture: %s, flags 0x%08x:\n", architecture_name(f->eh->e_machine), flags);
    int first = 1;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (flags & names[i].bit) {
            printf("%s%s", first ? "" : ", ", names[i].name);
            first = 0;
        }
    printf("\nstart address 0x%016llx\n", u64(f->eh->e_entry));
}

/* ---- archive headers ---- */

static void print_archive_member(const struct ar_member *m)
{
    char mode[10];
    static const char letters[] = "rwxrwxrwx";
    for (int i = 0; i < 9; i++)
        mode[i] = (m->mode & (0400u >> i)) ? letters[i] : '-';
    mode[9] = '\0';
    time_t t = (time_t)m->date;
    char *when = ctime(&t);
    printf("%s %d/%d %6lu %.12s %.4s %s\n", mode, m->uid, m->gid, (unsigned long)m->size, when ? when + 4 : "",
           when ? when + 20 : "", m->name);
}

/* ---- private headers ---- */

static const char *program_type_name(uint32_t type, char *buf, size_t len)
{
    switch (type) {
    case PT_NULL:         return "NULL";
    case PT_LOAD:         return "LOAD";
    case PT_DYNAMIC:      return "DYNAMIC";
    case PT_INTERP:       return "INTERP";
    case PT_NOTE:         return "NOTE";
    case PT_SHLIB:        return "SHLIB";
    case PT_PHDR:         return "PHDR";
    case PT_TLS:          return "TLS";
    case PT_GNU_EH_FRAME: return "EH_FRAME";
    case PT_GNU_STACK:    return "STACK";
    case PT_GNU_RELRO:    return "RELRO";
    default:
        snprintf(buf, len, "0x%08x", type);
        return buf;
    }
}

static void dump_program_headers(const struct elffile *f)
{
    printf("\nProgram Header:\n");
    for (unsigned i = 0; i < f->nsegments; i++) {
        const Elf64_Phdr *p = &f->ph[i];
        char buf[16];
        printf("%8s off    0x%016llx vaddr 0x%016llx paddr 0x%016llx align 2**%u\n",
               program_type_name(p->p_type, buf, sizeof buf), u64(p->p_offset), u64(p->p_vaddr), u64(p->p_paddr),
               section_alignment_power(p->p_align));
        printf("         filesz 0x%016llx memsz 0x%016llx flags %c%c%c", u64(p->p_filesz), u64(p->p_memsz),
               (p->p_flags & PF_R) ? 'r' : '-', (p->p_flags & PF_W) ? 'w' : '-', (p->p_flags & PF_X) ? 'x' : '-');
        uint32_t extra = p->p_flags & ~(uint32_t)(PF_R | PF_W | PF_X);
        if (extra)
            printf(" %lx", (unsigned long)extra);
        printf("\n");
    }
}

/* dynamic_tag_name returns the tag name of BFD, or NULL for a tag that objdump prints in hexadecimal. */
static const char *dynamic_tag_name(int64_t tag)
{
    if (tag == DT_FEATURE_1)
        return "FEATURE";
    if (tag == 0x7ffffffd)
        return "AUXILIARY";
    if (tag == 0x7ffffffe)
        return "USED";
    if (tag == 0x7fffffff)
        return "FILTER";
    if (tag == DT_TLSDESC_PLT || tag == DT_TLSDESC_GOT)
        return NULL;
    return elffile_dynamic_tag_name(EM_X86_64, tag);
}

static void dump_dynamic_section(const struct elffile *f)
{
    const Elf64_Shdr *dyn = NULL;
    for (unsigned i = 0; i < f->nsections; i++)
        if (f->sh[i].sh_type == SHT_DYNAMIC) {
            dyn = &f->sh[i];
            break;
        }
    if (!dyn)
        return;
    const Elf64_Dyn *entries = elffile_section_data(f, dyn);
    const Elf64_Shdr *strtab = elffile_linked(f, dyn);
    if (!entries)
        return;
    printf("\nDynamic Section:\n");
    size_t n = dyn->sh_size / sizeof *entries;
    for (size_t i = 0; i < n && entries[i].d_tag != DT_NULL; i++) {
        int64_t tag = entries[i].d_tag;
        uint64_t val = entries[i].d_un.d_val;
        const char *name = dynamic_tag_name(tag);
        char buf[32];
        if (!name) {
            snprintf(buf, sizeof buf, "0x%llx", u64((uint64_t)tag));
            name = buf;
        }
        printf("  %-20s ", name);
        int is_string = tag == DT_NEEDED || tag == DT_SONAME || tag == DT_RPATH || tag == DT_RUNPATH ||
                        tag == 0x7ffffffd || tag == 0x7fffffff || tag == DT_AUDIT || tag == DT_DEPAUDIT;
        const char *s = is_string ? elffile_string(f, strtab, val) : NULL;
        if (s)
            printf("%s\n", s);
        else
            printf("0x%016llx\n", u64(val));
    }
}

static void dump_version_definitions(const struct elffile *f)
{
    for (unsigned i = 0; i < f->nsections; i++) {
        const Elf64_Shdr *s = &f->sh[i];
        if (s->sh_type != SHT_GNU_verdef)
            continue;
        const uint8_t *data = elffile_section_data(f, s);
        const Elf64_Shdr *strtab = elffile_linked(f, s);
        if (!data)
            continue;
        printf("\nVersion definitions:\n");
        uint64_t off = 0;
        for (unsigned k = 0; k < s->sh_info; k++) {
            if (off > s->sh_size || s->sh_size - off < sizeof(Elf64_Verdef))
                break;
            Elf64_Verdef d;
            memcpy(&d, data + off, sizeof d);
            uint64_t aoff = off + d.vd_aux;
            Elf64_Verdaux a;
            const char *name = "<corrupt>";
            if (aoff <= s->sh_size && s->sh_size - aoff >= sizeof a) {
                memcpy(&a, data + aoff, sizeof a);
                const char *str = elffile_string(f, strtab, a.vda_name);
                if (str)
                    name = str;
            }
            printf("%d 0x%02x 0x%08x %s\n", d.vd_ndx, d.vd_flags, d.vd_hash, name);
            int extra = 0;
            for (unsigned j = 1; j < d.vd_cnt; j++) {
                if (a.vda_next == 0)
                    break;
                aoff += a.vda_next;
                if (aoff > s->sh_size || s->sh_size - aoff < sizeof a)
                    break;
                memcpy(&a, data + aoff, sizeof a);
                const char *str = elffile_string(f, strtab, a.vda_name);
                printf("\t%s ", str ? str : "<corrupt>");
                extra = 1;
            }
            if (extra)
                printf("\n");
            if (d.vd_next == 0)
                break;
            off += d.vd_next;
        }
    }
}

static void dump_version_references(const struct elffile *f)
{
    for (unsigned i = 0; i < f->nsections; i++) {
        const Elf64_Shdr *s = &f->sh[i];
        if (s->sh_type != SHT_GNU_verneed)
            continue;
        const uint8_t *data = elffile_section_data(f, s);
        const Elf64_Shdr *strtab = elffile_linked(f, s);
        if (!data)
            continue;
        printf("\nVersion References:\n");
        uint64_t off = 0;
        for (unsigned k = 0; k < s->sh_info; k++) {
            if (off > s->sh_size || s->sh_size - off < sizeof(Elf64_Verneed))
                break;
            Elf64_Verneed n;
            memcpy(&n, data + off, sizeof n);
            const char *file = elffile_string(f, strtab, n.vn_file);
            printf("  required from %s:\n", file ? file : "<corrupt>");
            uint64_t aoff = off + n.vn_aux;
            for (unsigned j = 0; j < n.vn_cnt; j++) {
                if (aoff > s->sh_size || s->sh_size - aoff < sizeof(Elf64_Vernaux))
                    break;
                Elf64_Vernaux a;
                memcpy(&a, data + aoff, sizeof a);
                const char *name = elffile_string(f, strtab, a.vna_name);
                printf("    0x%08x 0x%02x %02d %s\n", a.vna_hash, a.vna_flags, a.vna_other, name ? name : "<corrupt>");
                if (a.vna_next == 0)
                    break;
                aoff += a.vna_next;
            }
            if (n.vn_next == 0)
                break;
            off += n.vn_next;
        }
    }
}

static void dump_private_headers(const struct elffile *f)
{
    if (f->nsegments > 0)
        dump_program_headers(f);
    dump_dynamic_section(f);
    dump_version_definitions(f);
    dump_version_references(f);
    if (f->eh->e_machine == EM_AARCH64)
        printf("private flags = 0x%x:\n", f->eh->e_flags);
    printf("\n");
}

/* ---- symbols ---- */

enum {
    BSF_LOCAL = 1u << 0,
    BSF_GLOBAL = 1u << 1,
    BSF_DEBUGGING = 1u << 2,
    BSF_FUNCTION = 1u << 3,
    BSF_WEAK = 1u << 4,
    BSF_SECTION_SYM = 1u << 5,
    BSF_FILE = 1u << 6,
    BSF_OBJECT = 1u << 7,
    BSF_THREAD_LOCAL = 1u << 8,
    BSF_DYNAMIC = 1u << 9,
    BSF_GNU_UNIQUE = 1u << 10,
    BSF_INDIRECT_FUNCTION = 1u << 11,
};

/* is_special_symbol reports the mapping symbols of aarch64 that objdump leaves out. */
static int is_special_symbol(const struct elffile *f, const char *name)
{
    return f->eh->e_machine == EM_AARCH64 && name[0] == '$' && (name[1] == 'x' || name[1] == 'd') &&
           (name[2] == '\0' || name[2] == '.');
}

static unsigned symbol_flags(const Elf64_Sym *s, int dynamic)
{
    unsigned flags = 0;
    switch (ELF64_ST_BIND(s->st_info)) {
    case STB_LOCAL:
        flags |= BSF_LOCAL;
        break;
    case STB_GLOBAL:
        if (s->st_shndx != SHN_UNDEF && s->st_shndx != SHN_COMMON)
            flags |= BSF_GLOBAL;
        break;
    case STB_WEAK:
        flags |= BSF_WEAK;
        break;
    case STB_GNU_UNIQUE:
        flags |= BSF_GNU_UNIQUE;
        break;
    default:
        break;
    }
    switch (ELF64_ST_TYPE(s->st_info)) {
    case STT_SECTION:
        flags |= BSF_SECTION_SYM | BSF_DEBUGGING;
        break;
    case STT_FILE:
        flags |= BSF_FILE | BSF_DEBUGGING;
        break;
    case STT_FUNC:
        flags |= BSF_FUNCTION;
        break;
    case STT_COMMON:
    case STT_OBJECT:
        flags |= BSF_OBJECT;
        break;
    case STT_TLS:
        flags |= BSF_THREAD_LOCAL;
        break;
    case STT_GNU_IFUNC:
        flags |= BSF_INDIRECT_FUNCTION;
        break;
    default:
        break;
    }
    if (dynamic)
        flags |= BSF_DYNAMIC;
    return flags;
}

/* symbol_section_name returns the section name that objdump prints for a symbol. */
static const char *symbol_section_name(const struct elffile *f, const struct bfd_sections *b, const Elf64_Sym *s)
{
    if (s->st_shndx == SHN_ABS)
        return "*ABS*";
    if (s->st_shndx == SHN_COMMON)
        return "*COM*";
    if (s->st_shndx == SHN_UNDEF || s->st_shndx >= f->nsections)
        return "*UND*";
    for (size_t i = 0; i < b->count; i++)
        if (b->list[i].elf_index == s->st_shndx)
            return elffile_section_name(f, b->list[i].sh);
    return "*UND*";
}

/* symbol_version writes the version text of a dynamic symbol, as objdump prints it after the size. */
static void print_symbol_version(const struct elffile *f, const struct elfdump_versions *v, const Elf64_Sym *s,
                                 size_t n)
{
    char buf[80];
    const char *text = NULL;
    int hidden = 0;
    if (s && v->versym && n < v->nversym) {
        unsigned data = v->versym[n], index = data & 0x7fff;
        hidden = (data & 0x8000) != 0;
        if (index == 1) {
            text = "Base";
        } else if (index > 1) {
            int is_need;
            text = elfdump_version_name(f, v, index, &is_need);
            if (is_need)
                hidden = 1;
            if (s->st_shndx == SHN_UNDEF && !is_need)
                text = NULL;
        }
    }
    if (!text) {
        printf("             ");
        return;
    }
    if (hidden) {
        snprintf(buf, sizeof buf, " (%s)", text);
        printf("%-13s", buf);
    } else {
        printf("  %-11s", text);
    }
}

static void dump_symbol_table(const struct elffile *f, const struct bfd_sections *b, const Elf64_Shdr *symtab,
                              int dynamic)
{
    size_t count = 0;
    const Elf64_Shdr *strtab = NULL;
    const Elf64_Sym *syms = symtab ? elffile_symbols(f, symtab, &count, &strtab) : NULL;
    if (!syms || count <= 1) {
        printf("%s\nno symbols\n\n\n", dynamic ? "DYNAMIC SYMBOL TABLE:" : "SYMBOL TABLE:");
        return;
    }
    printf("%s\n", dynamic ? "DYNAMIC SYMBOL TABLE:" : "SYMBOL TABLE:");
    struct elfdump_versions v;
    memset(&v, 0, sizeof v);
    if (dynamic)
        elfdump_versions_init(f, symtab, &v);
    struct elfdump_versions dv;
    const Elf64_Shdr *dynsym = elffile_find_type(f, SHT_DYNSYM);
    memset(&dv, 0, sizeof dv);
    if (dynsym)
        elfdump_versions_init(f, dynsym, &dv);
    int have_versions = dv.versym && (dv.verdef_sh || dv.verneed_sh);
    for (size_t i = 1; i < count; i++) {
        const Elf64_Sym *s = &syms[i];
        const char *name = elffile_symbol_name(f, strtab, s);
        if (is_special_symbol(f, name))
            continue;
        const char *secname = symbol_section_name(f, b, s);
        if (nonly) {
            int hit = 0;
            for (size_t k = 0; k < nonly; k++)
                if (strcmp(only[k], secname) == 0) {
                    hit = 1;
                    only_seen[k] = 1;
                }
            if (!hit)
                continue;
        }
        unsigned fl = symbol_flags(s, dynamic);
        uint64_t value = s->st_value;
        if (s->st_shndx == SHN_COMMON)
            value = s->st_size;
        printf("%016llx %c%c%c%c%c%c%c %s\t", u64(value),
               (fl & BSF_LOCAL) && (fl & BSF_GLOBAL) ? '!' : (fl & BSF_LOCAL) ? 'l' : (fl & BSF_GLOBAL) ? 'g' :
               (fl & BSF_GNU_UNIQUE) ? 'u' : ' ',
               (fl & BSF_WEAK) ? 'w' : ' ', ' ', ' ', (fl & BSF_INDIRECT_FUNCTION) ? 'i' : ' ',
               (fl & BSF_DEBUGGING) ? 'd' : (fl & BSF_DYNAMIC) ? 'D' : ' ',
               (fl & BSF_FILE) ? 'f' : (fl & BSF_FUNCTION) ? 'F' : (fl & BSF_OBJECT) ? 'O' : ' ',
               secname);
        printf("%016llx", u64(s->st_shndx == SHN_COMMON ? s->st_value : s->st_size));
        if (have_versions)
            print_symbol_version(f, dynamic ? &v : &dv, dynamic ? s : NULL, i);
        switch (s->st_other) {
        case 0:             break;
        case STV_INTERNAL:  printf(" .internal"); break;
        case STV_HIDDEN:    printf(" .hidden"); break;
        case STV_PROTECTED: printf(" .protected"); break;
        default:            printf(" 0x%02x", s->st_other); break;
        }
        printf(" %s\n", name);
    }
    printf("\n\n");
}

/* ---- relocations ---- */

/* reloc_symbol_text writes the symbol and the addend of a relocation in the manner of objdump. */
static void print_reloc_value(const struct elffile *f, const struct bfd_sections *b, const Elf64_Shdr *symtab,
                              int dynamic, const struct elfdump_reloc *r)
{
    uint32_t symidx = ELF64_R_SYM(r->info);
    size_t nsyms = 0;
    const Elf64_Shdr *strtab = NULL;
    const Elf64_Sym *syms = symtab ? elffile_symbols(f, symtab, &nsyms, &strtab) : NULL;
    (void)b;
    if (symidx == 0 || !syms || symidx >= nsyms) {
        printf("*ABS*");
    } else if (dynamic) {
        struct elfdump_versions v;
        elfdump_versions_init(f, symtab, &v);
        printf("%s", elfdump_format_symbol_name(f, &v, strtab, &syms[symidx], symidx, 0));
    } else {
        printf("%s", elffile_symbol_name(f, strtab, &syms[symidx]));
    }
    if (r->addend < 0)
        printf("-0x%016llx", u64((uint64_t)-r->addend));
    else if (r->addend > 0)
        printf("+0x%016llx", u64((uint64_t)r->addend));
}

static void print_reloc_row(const struct elffile *f, const struct bfd_sections *b, const Elf64_Shdr *symtab,
                            int dynamic, const struct elfdump_reloc *r)
{
    const char *type = elffile_reloc_name(f->eh->e_machine, ELF64_R_TYPE(r->info));
    printf("%016llx %-16s  ", u64(r->offset), type ? type : "*unknown*");
    print_reloc_value(f, b, symtab, dynamic, r);
    printf("\n");
}

static void dump_relocs(const struct elffile *f, const struct bfd_sections *b)
{
    for (size_t i = 0; i < b->count; i++) {
        const struct bfd_section *sec = &b->list[i];
        if (!section_selected(f, sec) || !(sec->flags & SEC_RELOC))
            continue;
        printf("RELOCATION RECORDS FOR [%s]:\n", elffile_section_name(f, sec->sh));
        printf("OFFSET           TYPE              VALUE\n");
        for (unsigned k = 0; k < f->nsections; k++) {
            const Elf64_Shdr *rs = &f->sh[k];
            if (!is_reloc_for_section(f, b->symtab, rs) || rs->sh_info != sec->elf_index)
                continue;
            struct elfdump_reloc *r;
            long n = elfdump_read_relocs(f, rs, &r);
            for (long j = 0; j < n; j++)
                print_reloc_row(f, b, b->symtab, 0, &r[j]);
            free(r);
        }
        printf("\n\n");
    }
}

static void dump_dynamic_relocs(const struct elffile *f, const struct bfd_sections *b)
{
    const Elf64_Shdr *dynsym = elffile_find_type(f, SHT_DYNSYM);
    int any = 0;
    if (dynsym) {
        unsigned dyn_index = elffile_section_index(f, dynsym);
        for (unsigned k = 0; k < f->nsections; k++) {
            const Elf64_Shdr *rs = &f->sh[k];
            if ((rs->sh_type != SHT_REL && rs->sh_type != SHT_RELA) || rs->sh_link != dyn_index)
                continue;
            struct elfdump_reloc *r;
            long n = elfdump_read_relocs(f, rs, &r);
            if (n > 0 && !any) {
                printf("DYNAMIC RELOCATION RECORDS\n");
                printf("OFFSET           TYPE              VALUE\n");
                any = 1;
            }
            for (long j = 0; j < n; j++)
                print_reloc_row(f, b, dynsym, 1, &r[j]);
            free(r);
        }
    }
    if (any)
        printf("\n\n");
    else
        printf("DYNAMIC RELOCATION RECORDS (none)\n\n");
}

/* ---- section contents ---- */

static void dump_contents(const struct elffile *f, const struct bfd_sections *b)
{
    for (size_t i = 0; i < b->count; i++) {
        const struct bfd_section *sec = &b->list[i];
        const Elf64_Shdr *s = sec->sh;
        if (!(sec->flags & SEC_HAS_CONTENTS) || !section_selected(f, sec))
            continue;
        const uint8_t *data = elffile_section_data(f, s);
        if (!data || s->sh_size == 0)
            continue;
        printf("Contents of section %s:\n", elffile_section_name(f, s));
        uint64_t end = s->sh_addr + s->sh_size - 1;
        int width = 4;
        while (width < 16 && (end >> (4 * width)) != 0)
            width++;
        for (uint64_t pos = 0; pos < s->sh_size; pos += 16) {
            size_t n = s->sh_size - pos < 16 ? (size_t)(s->sh_size - pos) : 16;
            printf(" %0*llx ", width, u64(s->sh_addr + pos));
            for (size_t j = 0; j < 16; j++) {
                if (j < n)
                    printf("%02x", data[pos + j]);
                else
                    printf("  ");
                if ((j & 3) == 3)
                    printf(" ");
            }
            printf(" ");
            for (size_t j = 0; j < 16; j++) {
                if (j < n)
                    putchar(data[pos + j] >= ' ' && data[pos + j] < 0x7f ? data[pos + j] : '.');
                else
                    putchar(' ');
            }
            printf("\n");
        }
    }
}

/* ---- driver ---- */

static int process_archive(const char *path, const struct archive *a, void *arg)
{
    (void)arg;
    current_archive = a;
    printf("In archive %s:\n", path);
    return 0;
}

static int process_object(const char *path, const char *member, const struct elffile *f, void *arg)
{
    (void)arg;
    const char *bfd_name = elffile_bfd_name(f->eh->e_machine);
    if (!bfd_name) {
        bu_error(path, "file format not recognized");
        return 1;
    }
    struct bfd_sections b;
    bfd_sections_load(f, &b);
    printf("\n%s:     file format %s\n", member ? member : path, bfd_name);
    if (opt.archive_headers) {
        if (member) {
            for (const struct ar_member *m = current_archive ? current_archive->members : NULL; m; m = m->next)
                if (strcmp(m->name, member) == 0) {
                    print_archive_member(m);
                    break;
                }
        } else {
            printf("%s\n", path);
        }
    }
    if (opt.file_headers)
        dump_file_header(f, &b);
    if (!opt.private_headers)
        printf("\n");
    if (opt.private_headers)
        dump_private_headers(f);
    if (opt.section_headers)
        dump_section_headers(f, &b);
    if (opt.syms)
        dump_symbol_table(f, &b, b.symtab, 0);
    if (opt.dyn_syms) {
        const Elf64_Shdr *dynsym = elffile_find_type(f, SHT_DYNSYM);
        if (!dynsym)
            bu_error(NULL, "%s: not a dynamic object", member ? member : path);
        dump_symbol_table(f, &b, dynsym, 1);
    }
    if (opt.relocs)
        dump_relocs(f, &b);
    if (opt.dyn_relocs) {
        if (!elffile_find_type(f, SHT_DYNSYM))
            bu_error(NULL, "%s: not a dynamic object", member ? member : path);
        dump_dynamic_relocs(f, &b);
    }
    if (opt.contents)
        dump_contents(f, &b);
    free(b.list);
    return 0;
}

static void usage(FILE *out)
{
    fprintf(out, "Usage: objdump <option(s)> <file(s)>\n"
                 " Display information from object <file(s)>.\n"
                 " At least one of the following switches must be given:\n"
                 "  -a, --archive-headers    Display archive header information\n"
                 "  -f, --file-headers       Display the contents of the overall file header\n"
                 "  -p, --private-headers    Display object format specific file header contents\n"
                 "  -h, --[section-]headers  Display the contents of the section headers\n"
                 "  -x, --all-headers        Display the contents of all headers\n"
                 "  -s, --full-contents      Display the full contents of all sections requested\n"
                 "  -t, --syms               Display the contents of the symbol table(s)\n"
                 "  -T, --dynamic-syms       Display the contents of the dynamic symbol table\n"
                 "  -r, --reloc              Display the relocation entries in the file\n"
                 "  -R, --dynamic-reloc      Display the dynamic relocation entries in the file\n"
                 "  -v, --version            Display this program's version number\n"
                 "  -H, --help               Display this information\n"
                 " The following switches are optional:\n"
                 "  -j, --section=NAME       Only display information for section NAME\n"
                 "  -w, --wide               Format output for more than 80 columns\n"
                 " Disassembly (-d, -D, -S) is not available.\n");
}

enum {
    OPT_DISASSEMBLE = 256,
    OPT_DISASSEMBLE_ALL,
    OPT_SOURCE,
};

int main(int argc, char **argv)
{
    bu_program = "objdump";
    static const struct option longopts[] = {
        { "archive-headers", no_argument, NULL, 'a' },
        { "file-headers", no_argument, NULL, 'f' },
        { "private-headers", no_argument, NULL, 'p' },
        { "section-headers", no_argument, NULL, 'h' },
        { "headers", no_argument, NULL, 'h' },
        { "all-headers", no_argument, NULL, 'x' },
        { "full-contents", no_argument, NULL, 's' },
        { "syms", no_argument, NULL, 't' },
        { "dynamic-syms", no_argument, NULL, 'T' },
        { "reloc", no_argument, NULL, 'r' },
        { "dynamic-reloc", no_argument, NULL, 'R' },
        { "section", required_argument, NULL, 'j' },
        { "wide", no_argument, NULL, 'w' },
        { "disassemble", optional_argument, NULL, OPT_DISASSEMBLE },
        { "disassemble-all", no_argument, NULL, OPT_DISASSEMBLE_ALL },
        { "source", no_argument, NULL, OPT_SOURCE },
        { "help", no_argument, NULL, 'H' },
        { "version", no_argument, NULL, 'v' },
        { NULL, 0, NULL, 0 },
    };
    int c;
    while ((c = getopt_long(argc, argv, "afphxstTrRj:wdDSHv", longopts, NULL)) != -1) {
        switch (c) {
        case 'a': opt.archive_headers = 1; break;
        case 'f': opt.file_headers = 1; break;
        case 'p': opt.private_headers = 1; break;
        case 'h': opt.section_headers = 1; break;
        case 'x':
            opt.archive_headers = opt.file_headers = opt.private_headers = opt.section_headers = 1;
            opt.syms = opt.relocs = 1;
            break;
        case 's': opt.contents = 1; break;
        case 't': opt.syms = 1; break;
        case 'T': opt.dyn_syms = 1; break;
        case 'r': opt.relocs = 1; break;
        case 'R': opt.dyn_relocs = 1; break;
        case 'w': opt.wide = 1; break;
        case 'j':
            only = bu_realloc(only, (nonly + 1) * sizeof *only);
            only[nonly++] = optarg;
            break;
        case 'd':
        case 'D':
        case 'S':
        case OPT_DISASSEMBLE:
        case OPT_DISASSEMBLE_ALL:
        case OPT_SOURCE:
            bu_error(NULL, "disassembly is not available (planned for a later release)");
            return 1;
        case 'v':
            printf("objdump (minios binutils) 0.5\n");
            return 0;
        case 'H':
            usage(stdout);
            return 0;
        default:
            usage(stderr);
            return 1;
        }
    }
    if (!(opt.archive_headers || opt.file_headers || opt.private_headers || opt.section_headers || opt.syms ||
          opt.dyn_syms || opt.relocs || opt.dyn_relocs || opt.contents)) {
        usage(stderr);
        return 1;
    }
    if ((argc - optind) == 0) {
        bu_error(NULL, "'a.out': No such file");
        return 1;
    }
    only_seen = bu_alloc((nonly + 1) * sizeof *only_seen);
    memset(only_seen, 0, (nonly + 1) * sizeof *only_seen);
    int status = 0;
    for (int i = 0; i < (argc - optind); i++)
        if (bu_for_each_object(argv[optind + i], process_archive, process_object, NULL, NULL))
            status = 1;
    for (size_t i = 0; i < nonly; i++)
        if (!only_seen[i]) {
            bu_error(NULL, "section '%s' mentioned in a -j option, but not found in any input file", only[i]);
            status = 1;
        }
    return status;
}
