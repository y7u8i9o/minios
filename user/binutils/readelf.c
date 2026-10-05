/* readelf: display the contents of ELF64 files in the wide format of the
 * GNU binutils.  The output equals the output of GNU readelf -W for the
 * options that the program implements.
 *
 *     readelf [-aeghlSsrdnuVAIW] [-x section] [-p section] file...
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

/* The kinds of output that the options select. */
struct options {
    int header, segments, sections, groups, syms, dyn_syms, notes, relocs, unwind, dynamic;
    int version, arch, histogram, got;
};

/* One request of -x or -p.  The request matches sections by number or name. */
struct dump_request {
    const char *spec;
    int hex;                    /* 1 for -x, 0 for -p */
};

static struct options opt;
static struct dump_request *requests;
static size_t nrequests;
static int show_names;          /* print "File: NAME" before the output of each file */

static unsigned long long u64(uint64_t v)
{
    return (unsigned long long)v;
}

/* ---- names ---- */

static const char *file_type_name(unsigned type, char *buf, size_t len)
{
    const char *name = elffile_type_name(type);
    if (name)
        return name;
    if (type >= ET_LOPROC)
        snprintf(buf, len, "Processor Specific: (%x)", type);
    else if (type >= ET_LOOS)
        snprintf(buf, len, "OS Specific: (%x)", type);
    else
        snprintf(buf, len, "<unknown>: %x", type);
    return buf;
}

static const char *section_type_name(unsigned machine, uint32_t type, char *buf, size_t len)
{
    const char *name = elffile_section_type_name(machine, type);
    if (name)
        return name;
    if (type >= 0x70000000 && type <= 0x7fffffff)
        snprintf(buf, len, "LOPROC+%x", type - 0x70000000);
    else if (type >= 0x60000000 && type <= 0x6fffffff)
        snprintf(buf, len, "LOOS+%x", type - 0x60000000);
    else if (type >= 0x80000000)
        snprintf(buf, len, "LOUSER+%x", type - 0x80000000);
    else
        snprintf(buf, len, "<unknown>: %x", type);
    return buf;
}

static const char *segment_type_name(unsigned machine, uint32_t type, char *buf, size_t len)
{
    const char *name = elffile_segment_type_name(machine, type);
    if (name)
        return name;
    if (type >= 0x70000000 && type <= 0x7fffffff)
        snprintf(buf, len, "LOPROC+%x", type - 0x70000000);
    else if (type >= 0x60000000 && type <= 0x6fffffff)
        snprintf(buf, len, "LOOS+%x", type - 0x60000000);
    else
        snprintf(buf, len, "<unknown>: %x", type);
    return buf;
}

/* section_flags writes the letters of the flags of a section header. */
static const char *section_flags(unsigned machine, uint64_t flags, char *buf, size_t len)
{
    size_t n = 0;
    while (flags && n + 1 < len) {
        uint64_t bit = flags & -flags;
        flags &= ~bit;
        char c;
        switch (bit) {
        case SHF_WRITE:             c = 'W'; break;
        case SHF_ALLOC:             c = 'A'; break;
        case SHF_EXECINSTR:         c = 'X'; break;
        case SHF_MERGE:             c = 'M'; break;
        case SHF_STRINGS:           c = 'S'; break;
        case SHF_INFO_LINK:         c = 'I'; break;
        case SHF_LINK_ORDER:        c = 'L'; break;
        case SHF_OS_NONCONFORMING:  c = 'O'; break;
        case SHF_GROUP:             c = 'G'; break;
        case SHF_TLS:               c = 'T'; break;
        case SHF_EXCLUDE:           c = 'E'; break;
        case SHF_COMPRESSED:        c = 'C'; break;
        default:
            if (machine == EM_X86_64 && bit == SHF_X86_64_LARGE) {
                c = 'l';
            } else if (bit & SHF_MASKOS) {
                if (bit == 0x01000000)
                    c = 'D';
                else if (bit == SHF_GNU_RETAIN)
                    c = 'R';
                else
                    c = 'o';
                flags &= ~(uint64_t)SHF_MASKOS;
                if (c == 'o')
                    flags &= ~(uint64_t)SHF_MASKOS;
            } else if (bit & SHF_MASKPROC) {
                c = 'p';
                flags &= ~(uint64_t)SHF_MASKPROC;
            } else {
                c = 'x';
            }
        }
        buf[n++] = c;
    }
    buf[n] = '\0';
    return buf;
}

/* ---- file header ---- */

static void print_file_header(const struct elffile *f)
{
    const Elf64_Ehdr *eh = f->eh;
    char buf[64];
    printf("ELF Header:\n  Magic:   ");
    for (int i = 0; i < EI_NIDENT; i++)
        printf("%2.2x ", eh->e_ident[i]);
    printf("\n");
    printf("  Class:                             ELF64\n");
    printf("  Data:                              2's complement, little endian\n");
    printf("  Version:                           %d%s\n", eh->e_ident[EI_VERSION],
           eh->e_ident[EI_VERSION] == EV_CURRENT ? " (current)" : "");
    const char *osabi = elffile_osabi_name(eh->e_ident[EI_OSABI]);
    if (osabi)
        printf("  OS/ABI:                            %s\n", osabi);
    else
        printf("  OS/ABI:                            <unknown: %x>\n", eh->e_ident[EI_OSABI]);
    printf("  ABI Version:                       %d\n", eh->e_ident[EI_ABIVERSION]);
    printf("  Type:                              %s\n", file_type_name(eh->e_type, buf, sizeof buf));
    const char *machine = elffile_machine_name(eh->e_machine);
    if (machine)
        printf("  Machine:                           %s\n", machine);
    else
        printf("  Machine:                           <unknown>: 0x%x\n", eh->e_machine);
    printf("  Version:                           0x%x\n", eh->e_version);
    printf("  Entry point address:               0x%llx\n", u64(eh->e_entry));
    printf("  Start of program headers:          %llu (bytes into file)\n", u64(eh->e_phoff));
    printf("  Start of section headers:          %llu (bytes into file)\n", u64(eh->e_shoff));
    printf("  Flags:                             0x%x\n", eh->e_flags);
    printf("  Size of this header:               %d (bytes)\n", eh->e_ehsize);
    printf("  Size of program headers:           %d (bytes)\n", eh->e_phentsize);
    printf("  Number of program headers:         %d", eh->e_phnum);
    if (eh->e_phnum == PN_XNUM && f->sh && f->nsections > 0 && f->sh[0].sh_info != 0)
        printf(" (%d)", f->sh[0].sh_info);
    printf("\n");
    printf("  Size of section headers:           %d (bytes)\n", eh->e_shentsize);
    printf("  Number of section headers:         %d", eh->e_shnum);
    if (eh->e_shnum == 0 && f->sh && f->nsections > 0 && f->sh[0].sh_size != 0)
        printf(" (%llu)", u64(f->sh[0].sh_size));
    printf("\n");
    printf("  Section header string table index: %d", eh->e_shstrndx);
    if (eh->e_shstrndx == SHN_XINDEX && f->sh && f->nsections > 0)
        printf(" (%u)", f->sh[0].sh_link);
    else if (eh->e_shstrndx != SHN_UNDEF && eh->e_shstrndx >= (f->nsections ? f->nsections : eh->e_shnum))
        printf(" <corrupt: out of range>");
    printf("\n");
}

/* ---- section headers ---- */

static void print_section_headers(const struct elffile *f)
{
    const Elf64_Ehdr *eh = f->eh;
    if (eh->e_shnum == 0) {
        if (eh->e_shoff == 0)
            printf("\nThere are no sections in this file.\n");
        return;
    }
    if (!opt.header)
        printf("There %s %d section header%s, starting at offset 0x%llx:\n", eh->e_shnum == 1 ? "is" : "are",
               eh->e_shnum, eh->e_shnum == 1 ? "" : "s", u64(eh->e_shoff));
    if (!f->sh)
        return;
    printf("\nSection Headers:\n");
    printf("  [Nr] Name              Type            Address          Off    Size   ES Flg Lk Inf Al\n");
    for (unsigned i = 0; i < f->nsections; i++) {
        const Elf64_Shdr *s = &f->sh[i];
        char tbuf[32], fbuf[32];
        const char *name = f->shstrtab ? elffile_string(f, f->shstrtab, s->sh_name) : NULL;
        if (!name)
            name = f->shstrtab ? "<corrupt>" : "";
        printf("  [%2u] %-17s %-15s %016llx %6.6llx %6.6llx %2.2llx %3s %2u %3u %2llu\n", i, name,
               section_type_name(eh->e_machine, s->sh_type, tbuf, sizeof tbuf), u64(s->sh_addr), u64(s->sh_offset),
               u64(s->sh_size), u64(s->sh_entsize), section_flags(eh->e_machine, s->sh_flags, fbuf, sizeof fbuf),
               s->sh_link, s->sh_info, u64(s->sh_addralign));
    }
    printf("Key to Flags:\n");
    printf("  W (write), A (alloc), X (execute), M (merge), S (strings), I (info),\n");
    printf("  L (link order), O (extra OS processing required), G (group), T (TLS),\n");
    printf("  C (compressed), x (unknown), o (OS specific), E (exclude),\n");
    printf("  ");
    if (eh->e_ident[EI_OSABI] == ELFOSABI_GNU || eh->e_ident[EI_OSABI] == ELFOSABI_FREEBSD)
        printf("R (retain), ");
    printf("D (mbind), ");
    if (eh->e_machine == EM_X86_64)
        printf("l (large), ");
    printf("p (processor specific)\n");
}

/* ---- section groups ---- */

static void print_section_groups(const struct elffile *f)
{
    int found = 0;
    for (unsigned i = 0; i < f->nsections; i++) {
        const Elf64_Shdr *s = &f->sh[i];
        if (s->sh_type != SHT_GROUP)
            continue;
        found = 1;
        const uint32_t *words = elffile_section_data(f, s);
        size_t n = s->sh_size / 4;
        if (!words || n == 0)
            continue;
        const char *signature = "";
        size_t nsyms;
        const Elf64_Shdr *strtab;
        const Elf64_Sym *syms = elffile_symbols(f, elffile_linked(f, s), &nsyms, &strtab);
        if (syms && s->sh_info < nsyms)
            signature = elffile_symbol_name(f, strtab, &syms[s->sh_info]);
        uint32_t flags = words[0];
        const char *flag_name = "";
        if (flags & 1)
            flag_name = "COMDAT ";
        else if (flags != 0)
            flag_name = "<unknown> ";
        printf("\n%sgroup section [%5u] `%s' [%s] contains %zu sections:\n", flag_name, i,
               elffile_section_name(f, s), signature, n - 1);
        printf("   [Index]    Name\n");
        for (size_t k = 1; k < n; k++) {
            const Elf64_Shdr *m = elffile_section(f, words[k]);
            printf("   [%5u]   %s\n", words[k], m ? elffile_section_name(f, m) : "<corrupt>");
        }
    }
    if (!found)
        printf("\nThere are no section groups in this file.\n");
}

/* ---- program headers ---- */

/* The test of readelf for a section inside a segment (strict). */
static int section_in_segment(const Elf64_Shdr *s, const Elf64_Phdr *p)
{
    int tls = (s->sh_flags & SHF_TLS) != 0;
    int tbss = tls && s->sh_type == SHT_NOBITS;
    if (tbss && p->p_type != PT_TLS)
        return 0;
    if (tls) {
        if (p->p_type != PT_TLS && p->p_type != PT_GNU_RELRO && p->p_type != PT_LOAD)
            return 0;
    } else if (p->p_type == PT_TLS || p->p_type == PT_PHDR) {
        return 0;
    }
    if (!(s->sh_flags & SHF_ALLOC) && (p->p_type == PT_LOAD || p->p_type == PT_DYNAMIC ||
                                       p->p_type == PT_GNU_EH_FRAME || p->p_type == PT_GNU_STACK ||
                                       p->p_type == PT_GNU_RELRO || p->p_type == PT_GNU_SFRAME))
        return 0;
    uint64_t size = (!tls || s->sh_type != SHT_NOBITS || p->p_type == PT_TLS) ? s->sh_size : 0;
    if (s->sh_type != SHT_NOBITS) {
        if (s->sh_offset < p->p_offset)
            return 0;
        if (s->sh_offset - p->p_offset > p->p_filesz - 1 || p->p_filesz == 0)
            return 0;
        if (s->sh_offset - p->p_offset + size > p->p_filesz)
            return 0;
    }
    if (s->sh_flags & SHF_ALLOC) {
        if (s->sh_addr < p->p_vaddr)
            return 0;
        if (p->p_memsz == 0 || s->sh_addr - p->p_vaddr > p->p_memsz - 1)
            return 0;
        if (s->sh_addr + size - p->p_vaddr > p->p_memsz)
            return 0;
    }
    if ((p->p_type == PT_DYNAMIC || p->p_type == PT_NOTE) && s->sh_size == 0 && p->p_memsz != 0) {
        int file_ok = s->sh_type == SHT_NOBITS || (s->sh_offset > p->p_offset && s->sh_offset - p->p_offset < p->p_filesz);
        int vma_ok = !(s->sh_flags & SHF_ALLOC) || (s->sh_addr > p->p_vaddr && s->sh_addr - p->p_vaddr < p->p_memsz);
        if (!(file_ok && vma_ok))
            return 0;
    }
    return 1;
}

static void print_program_headers(const struct elffile *f)
{
    const Elf64_Ehdr *eh = f->eh;
    char buf[48];
    if (eh->e_phnum == 0) {
        if (eh->e_phoff == 0)
            printf("\nThere are no program headers in this file.\n");
        return;
    }
    if (!opt.header) {
        printf("\nElf file type is %s\n", file_type_name(eh->e_type, buf, sizeof buf));
        printf("Entry point 0x%llx\n", u64(eh->e_entry));
        printf("There %s %d program header%s, starting at offset %llu\n", eh->e_phnum == 1 ? "is" : "are",
               eh->e_phnum, eh->e_phnum == 1 ? "" : "s", u64(eh->e_phoff));
    }
    if (!f->ph)
        return;
    printf("\nProgram Headers:\n");
    printf("  Type           Offset   VirtAddr           PhysAddr           FileSiz  MemSiz   Flg Align\n");
    for (unsigned i = 0; i < f->nsegments; i++) {
        const Elf64_Phdr *p = &f->ph[i];
        printf("  %-14s 0x%6.6llx 0x%16.16llx 0x%16.16llx 0x%6.6llx 0x%6.6llx %c%c%c 0x%llx\n",
               segment_type_name(eh->e_machine, p->p_type, buf, sizeof buf), u64(p->p_offset), u64(p->p_vaddr),
               u64(p->p_paddr), u64(p->p_filesz), u64(p->p_memsz), (p->p_flags & PF_R) ? 'R' : ' ',
               (p->p_flags & PF_W) ? 'W' : ' ', (p->p_flags & PF_X) ? 'E' : ' ', u64(p->p_align));
        if (p->p_type == PT_INTERP) {
            printf("      [Requesting program interpreter: ");
            if (p->p_offset < f->size) {
                const char *s = (const char *)f->data + p->p_offset;
                size_t max = f->size - p->p_offset;
                if (max > p->p_filesz)
                    max = p->p_filesz;
                printf("%.*s", (int)strnlen(s, max), s);
            }
            printf("]\n");
        }
    }
    if (!f->sh || !f->shstrtab)
        return;
    printf("\n Section to Segment mapping:\n  Segment Sections...\n");
    for (unsigned i = 0; i < f->nsegments; i++) {
        const Elf64_Phdr *p = &f->ph[i];
        printf("   %2.2d     ", i);
        for (unsigned j = 1; j < f->nsections; j++)
            if (section_in_segment(&f->sh[j], p))
                printf("%s ", elffile_section_name(f, &f->sh[j]));
        printf("\n");
    }
}

/* ---- dynamic section ---- */

static void print_dt_flags(uint64_t flags)
{
    static const struct {
        uint64_t bit;
        const char *name;
    } names[] = {
        { DF_ORIGIN, "ORIGIN" }, { DF_SYMBOLIC, "SYMBOLIC" }, { DF_TEXTREL, "TEXTREL" },
        { DF_BIND_NOW, "BIND_NOW" }, { DF_STATIC_TLS, "STATIC_TLS" },
    };
    int first = 1;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (flags & names[i].bit) {
            printf("%s%s", first ? "" : " ", names[i].name);
            first = 0;
            flags &= ~names[i].bit;
        }
    if (flags)
        printf("%sunknown", first ? "" : " ");
    printf("\n");
}

static void print_dt_flags_1(uint64_t val)
{
    static const struct {
        uint64_t bit;
        const char *name;
    } names[] = {
        { DF_1_NOW, "NOW" }, { DF_1_GLOBAL, "GLOBAL" }, { DF_1_GROUP, "GROUP" }, { DF_1_NODELETE, "NODELETE" },
        { DF_1_LOADFLTR, "LOADFLTR" }, { DF_1_INITFIRST, "INITFIRST" }, { DF_1_NOOPEN, "NOOPEN" },
        { DF_1_ORIGIN, "ORIGIN" }, { DF_1_DIRECT, "DIRECT" }, { DF_1_TRANS, "TRANS" },
        { DF_1_INTERPOSE, "INTERPOSE" }, { DF_1_NODEFLIB, "NODEFLIB" }, { DF_1_NODUMP, "NODUMP" },
        { DF_1_CONFALT, "CONFALT" }, { DF_1_ENDFILTEE, "ENDFILTEE" }, { DF_1_DISPRELDNE, "DISPRELDNE" },
        { DF_1_DISPRELPND, "DISPRELPND" }, { DF_1_NODIRECT, "NODIRECT" }, { DF_1_IGNMULDEF, "IGNMULDEF" },
        { DF_1_NOKSYMS, "NOKSYMS" }, { DF_1_NOHDR, "NOHDR" }, { DF_1_EDITED, "EDITED" },
        { DF_1_NORELOC, "NORELOC" }, { DF_1_SYMINTPOSE, "SYMINTPOSE" }, { DF_1_GLOBAUDIT, "GLOBAUDIT" },
        { DF_1_SINGLETON, "SINGLETON" }, { DF_1_STUB, "STUB" }, { DF_1_PIE, "PIE" },
    };
    printf("Flags:");
    if (val == 0) {
        printf(" None\n");
        return;
    }
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++)
        if (val & names[i].bit) {
            printf(" %s", names[i].name);
            val &= ~names[i].bit;
        }
    if (val)
        printf(" %llx", u64(val));
    printf("\n");
}

static void print_dynamic_tag_type(unsigned machine, int64_t tag, char *out, size_t len)
{
    const char *name = elffile_dynamic_tag_name(machine, tag);
    if (name) {
        snprintf(out, len, "(%s)", name);
        return;
    }
    if (tag >= DT_LOPROC && tag <= DT_HIPROC)
        snprintf(out, len, "Processor Specific: %llx", u64((uint64_t)tag));
    else if (tag >= DT_LOOS && tag <= DT_HIOS)
        snprintf(out, len, "Operating System specific: %llx", u64((uint64_t)tag));
    else
        snprintf(out, len, "<unknown>: %llx", u64((uint64_t)tag));
}

static void print_dynamic_value(const struct elffile *f, const struct elfdump_dynamic *d, const Elf64_Dyn *e)
{
    uint64_t val = e->d_un.d_val;
    const char *label = NULL;
    switch (e->d_tag) {
    case DT_NEEDED:   label = "Shared library"; break;
    case DT_SONAME:   label = "Library soname"; break;
    case DT_RPATH:    label = "Library rpath"; break;
    case DT_RUNPATH:  label = "Library runpath"; break;
    case 0x7ffffffd: label = "Auxiliary library"; break;
    case 0x7fffffff: label = "Filter library"; break;
    case DT_AUDIT:    label = "Audit library"; break;
    case DT_DEPAUDIT: label = "Dependency audit library"; break;
    default: break;
    }
    if (label) {
        const char *s = elfdump_dynamic_string(d, val);
        if (s)
            printf("%s: [%s]\n", label, s);
        else
            printf("0x%llx\n", u64(val));
        return;
    }
    switch (e->d_tag) {
    case DT_PLTRELSZ: case DT_RELASZ: case DT_STRSZ: case DT_RELSZ: case DT_RELAENT: case DT_SYMENT:
    case DT_RELENT: case DT_PLTPADSZ: case DT_MOVEENT: case DT_MOVESZ: case DT_INIT_ARRAYSZ:
    case DT_FINI_ARRAYSZ: case DT_GNU_CONFLICTSZ: case DT_GNU_LIBLISTSZ: case DT_SYMINSZ: case DT_SYMINENT:
    case DT_PREINIT_ARRAYSZ: case DT_RELRSZ: case DT_RELRENT:
        printf("%llu (bytes)\n", u64(val));
        break;
    case DT_VERDEFNUM: case DT_VERNEEDNUM: case DT_RELACOUNT: case DT_RELCOUNT:
        printf("%llu\n", u64(val));
        break;
    case DT_PLTREL: {
        const char *n = elffile_dynamic_tag_name(f->eh->e_machine, (int64_t)val);
        if (n) {
            printf("%s\n", n);
        } else {
            printf("<unknown>: %llx\n", u64(val));
        }
        break;
    }
    case DT_FLAGS:
        printf("Flags: ");
        print_dt_flags(val);
        break;
    case DT_FLAGS_1:
        print_dt_flags_1(val);
        break;
    case DT_FEATURE_1:
        printf("Flags:");
        if (val == 0)
            printf(" None");
        else {
            if (val & 1)
                printf(" PARINIT");
            if (val & 2)
                printf(" CONFEXP");
            if (val & ~(uint64_t)3)
                printf(" %llx", u64(val & ~(uint64_t)3));
        }
        printf("\n");
        break;
    case DT_POSFLAG_1:
        printf("Flags:");
        if (val & 1)
            printf(" LAZY");
        if (val & 2)
            printf(" GROUPPERM");
        if (val & ~(uint64_t)3)
            printf(" %llx", u64(val & ~(uint64_t)3));
        printf("\n");
        break;
    case DT_BIND_NOW:
        printf("\n");
        break;
    default:
        if (f->eh->e_machine == EM_AARCH64 && (e->d_tag == DT_AARCH64_BTI_PLT || e->d_tag == DT_AARCH64_PAC_PLT ||
                                               e->d_tag == DT_AARCH64_VARIANT_PCS))
            printf("\n");
        else
            printf("0x%llx\n", u64(val));
        break;
    }
}

static void print_dynamic_section(const struct elffile *f)
{
    struct elfdump_dynamic d;
    if (elfdump_load_dynamic(f, &d) < 0) {
        printf("\nThere is no dynamic section in this file.\n");
        return;
    }
    printf("\nDynamic section at offset 0x%llx contains %zu entr%s:\n", u64(d.offset), d.count,
           d.count == 1 ? "y" : "ies");
    printf("  Tag        Type                         Name/Value\n");
    for (size_t i = 0; i < d.count; i++) {
        char type[80];
        print_dynamic_tag_type(f->eh->e_machine, d.entries[i].d_tag, type, sizeof type);
        printf(" 0x%16.16llx %-20s ", u64((uint64_t)d.entries[i].d_tag), type);
        print_dynamic_value(f, &d, &d.entries[i]);
        if (d.entries[i].d_tag == DT_NULL)
            break;
    }
}

/* ---- symbols ---- */

static const char *symbol_index_name(const struct elffile *f, unsigned shndx, char *buf, size_t len)
{
    switch (shndx) {
    case SHN_UNDEF:  return "UND";
    case SHN_ABS:    return "ABS";
    case SHN_COMMON: return "COM";
    default: break;
    }
    if (shndx >= SHN_LOPROC && shndx <= SHN_HIPROC)
        snprintf(buf, len, "PRC[0x%04x]", shndx);
    else if (shndx >= SHN_LOOS && shndx <= SHN_HIOS)
        snprintf(buf, len, "OS [0x%04x]", shndx);
    else if (shndx >= SHN_LORESERVE)
        snprintf(buf, len, "RSV[0x%04x]", shndx);
    else if (shndx >= f->nsections)
        snprintf(buf, len, "bad section index[%3d]", shndx);
    else
        snprintf(buf, len, "%3d", shndx);
    return buf;
}

static const char *numbered_name(const char *name, const char *prefix, unsigned value, char *buf, size_t len)
{
    if (name)
        return name;
    snprintf(buf, len, "%s: %u", prefix, value);
    return buf;
}

static void print_symbol_table(const struct elffile *f, const Elf64_Shdr *symtab)
{
    size_t count;
    const Elf64_Shdr *strtab;
    const Elf64_Sym *syms = elffile_symbols(f, symtab, &count, &strtab);
    const char *name = elffile_section_name(f, symtab);
    if (!syms)
        return;
    printf("\nSymbol table '%s' contains %zu entr%s:\n", name, count, count == 1 ? "y" : "ies");
    printf("   Num:    Value          Size Type    Bind   Vis      Ndx Name\n");
    struct elfdump_versions v;
    int dynamic = symtab->sh_type == SHT_DYNSYM;
    if (dynamic)
        elfdump_versions_init(f, symtab, &v);
    const uint32_t *shndx_table = NULL;
    for (unsigned i = 0; i < f->nsections; i++)
        if (f->sh[i].sh_type == SHT_SYMTAB_SHNDX && f->sh[i].sh_link == elffile_section_index(f, symtab))
            shndx_table = elffile_section_data(f, &f->sh[i]);
    for (size_t i = 0; i < count; i++) {
        const Elf64_Sym *s = &syms[i];
        char b1[40], b2[40], b3[40], b4[40], b5[40];
        printf("%6zu: %16.16llx ", i, u64(s->st_value));
        if (s->st_size <= 99999)
            printf("%5llu", u64(s->st_size));
        else
            printf("0x%llx", u64(s->st_size));
        unsigned type = ELF64_ST_TYPE(s->st_info), bind = ELF64_ST_BIND(s->st_info);
        const char *type_name = elffile_symbol_type_name(type);
        if (!type_name)
            type_name = numbered_name(NULL, type >= STT_LOPROC ? "<processor specific>" :
                                      type >= STT_LOOS ? "<OS specific>" : "<unknown>", type, b1, sizeof b1);
        const char *bind_name = elffile_symbol_bind_name(bind);
        if (!bind_name)
            bind_name = numbered_name(NULL, bind >= STB_LOPROC ? "<processor specific>" :
                                      bind >= STB_LOOS ? "<OS specific>" : "<unknown>", bind, b2, sizeof b2);
        printf(" %-7s %-6s", type_name, bind_name);
        unsigned vis = ELF64_ST_VISIBILITY(s->st_other);
        const char *vis_name = elffile_symbol_visibility_name(vis);
        printf(" %-7s", vis_name ? vis_name : numbered_name(NULL, "<unknown>", vis, b3, sizeof b3));
        if (s->st_other ^ vis) {
            unsigned other = s->st_other ^ vis;
            if (f->eh->e_machine == EM_AARCH64 && (other & 0x80))
                printf(" [VARIANT_PCS] ");
            else
                printf(" [<other>: %x] ", other);
        }
        unsigned shndx = s->st_shndx;
        if (shndx == SHN_XINDEX && shndx_table && i < symtab->sh_size / sizeof(Elf64_Sym))
            shndx = shndx_table[i];
        printf(" %4s ", symbol_index_name(f, shndx, b4, sizeof b4));
        (void)b5;
        if (dynamic)
            printf("%s\n", elfdump_format_symbol_name(f, &v, strtab, s, i, 1));
        else
            printf("%s\n", elffile_symbol_name(f, strtab, s));
    }
}

static void print_symbols(const struct elffile *f)
{
    for (unsigned i = 0; i < f->nsections; i++) {
        const Elf64_Shdr *s = &f->sh[i];
        if ((s->sh_type == SHT_SYMTAB && opt.syms) || (s->sh_type == SHT_DYNSYM && opt.dyn_syms))
            print_symbol_table(f, s);
    }
}

/* ---- hash histograms ---- */

static void print_histogram(const unsigned long *counts, unsigned long maxlength, unsigned long nbuckets,
                            unsigned long nsyms, const char *section)
{
    if (section)
        printf("\nHistogram for `%s' bucket list length (total of %lu bucket%s):\n", section, nbuckets,
               nbuckets == 1 ? "" : "s");
    else
        printf("\nHistogram for bucket list length (total of %lu bucket%s):\n", nbuckets, nbuckets == 1 ? "" : "s");
    printf(" Length  Number     %% of total  Coverage\n");
    printf("      0  %-10lu (%5.1f%%)\n", counts[0], (counts[0] * 100.0) / nbuckets);
    unsigned long nonzero = 0;
    for (unsigned long j = 1; j <= maxlength; j++) {
        nonzero += counts[j] * j;
        printf("%7lu  %-10lu (%5.1f%%)    %5.1f%%\n", j, counts[j], (counts[j] * 100.0) / nbuckets,
               nsyms ? (nonzero * 100.0) / nsyms : 0.0);
    }
}

static void histogram_sysv(const struct elffile *f, const Elf64_Shdr *s)
{
    const uint32_t *w = elffile_section_data(f, s);
    if (!w || s->sh_size < 8)
        return;
    uint32_t nbucket = w[0], nchain = w[1];
    if (nbucket == 0 || (uint64_t)(2 + nbucket + nchain) * 4 > s->sh_size)
        return;
    const uint32_t *buckets = w + 2, *chains = w + 2 + nbucket;
    unsigned long *lengths = bu_alloc(nbucket * sizeof *lengths);
    unsigned long maxlength = 0, nsyms = 0;
    for (uint32_t b = 0; b < nbucket; b++) {
        lengths[b] = 0;
        uint32_t guard = 0;
        for (uint32_t si = buckets[b]; si > 0 && si < nchain && guard <= nchain; si = chains[si], guard++)
            lengths[b]++;
        nsyms += lengths[b];
        if (lengths[b] > maxlength)
            maxlength = lengths[b];
    }
    unsigned long *counts = bu_alloc((maxlength + 1) * sizeof *counts);
    memset(counts, 0, (maxlength + 1) * sizeof *counts);
    for (uint32_t b = 0; b < nbucket; b++)
        counts[lengths[b]]++;
    print_histogram(counts, maxlength, nbucket, nsyms, NULL);
    free(counts);
    free(lengths);
}

static void histogram_gnu(const struct elffile *f, const Elf64_Shdr *s)
{
    const uint8_t *data = elffile_section_data(f, s);
    if (!data || s->sh_size < 16)
        return;
    uint32_t hdr[4];
    memcpy(hdr, data, sizeof hdr);
    uint32_t nbuckets = hdr[0], symoffset = hdr[1], bloom_size = hdr[2];
    uint64_t bloom_bytes = (uint64_t)bloom_size * 8;
    if (nbuckets == 0 || 16 + bloom_bytes + (uint64_t)nbuckets * 4 > s->sh_size)
        return;
    const uint8_t *buckets = data + 16 + bloom_bytes;
    const uint8_t *chain = buckets + (uint64_t)nbuckets * 4;
    uint64_t nchain = (s->sh_size - 16 - bloom_bytes - (uint64_t)nbuckets * 4) / 4;
    unsigned long *lengths = bu_alloc(nbuckets * sizeof *lengths);
    unsigned long maxlength = 0, nsyms = 0;
    for (uint32_t b = 0; b < nbuckets; b++) {
        uint32_t first;
        memcpy(&first, buckets + (size_t)b * 4, 4);
        lengths[b] = 0;
        if (first >= symoffset) {
            uint64_t off = first - symoffset;
            uint32_t value;
            do {
                if (off >= nchain)
                    break;
                memcpy(&value, chain + off * 4, 4);
                lengths[b]++;
                off++;
            } while (!(value & 1));
        }
        nsyms += lengths[b];
        if (lengths[b] > maxlength)
            maxlength = lengths[b];
    }
    unsigned long *counts = bu_alloc((maxlength + 1) * sizeof *counts);
    memset(counts, 0, (maxlength + 1) * sizeof *counts);
    for (uint32_t b = 0; b < nbuckets; b++)
        counts[lengths[b]]++;
    print_histogram(counts, maxlength, nbuckets, nsyms, elffile_section_name(f, s));
    free(counts);
    free(lengths);
}

static void print_histograms(const struct elffile *f)
{
    for (unsigned i = 0; i < f->nsections; i++)
        if (f->sh[i].sh_type == SHT_HASH)
            histogram_sysv(f, &f->sh[i]);
    for (unsigned i = 0; i < f->nsections; i++)
        if (f->sh[i].sh_type == SHT_GNU_HASH)
            histogram_gnu(f, &f->sh[i]);
}

/* ---- relocations ---- */

static const char *reloc_type_name(unsigned machine, uint32_t type, char *buf, size_t len)
{
    const char *name = elffile_reloc_name(machine, type);
    if (name)
        return name;
    snprintf(buf, len, "unrecognized: %-7x", type);
    return buf;
}

static void print_reloc_symbol(const struct elffile *f, const struct elfdump_versions *v, const Elf64_Sym *syms,
                               size_t nsyms, const Elf64_Shdr *strtab, int dynamic, uint32_t symidx)
{
    if (symidx >= nsyms) {
        printf("<corrupt>");
        return;
    }
    const Elf64_Sym *s = &syms[symidx];
    printf("%16.16llx ", u64(s->st_value));
    if (dynamic)
        printf("%s", elfdump_format_symbol_name(f, v, strtab, s, symidx, 0));
    else
        printf("%s", elffile_symbol_name(f, strtab, s));
}

static void print_reloc_section(const struct elffile *f, const Elf64_Shdr *sh)
{
    struct elfdump_reloc *r;
    long n = elfdump_read_relocs(f, sh, &r);
    if (n < 0)
        return;
    int rela = sh->sh_type == SHT_RELA;
    printf("\nRelocation section '%s' at offset 0x%llx contains %ld entr%s:\n", elffile_section_name(f, sh),
           u64(sh->sh_offset), n, n == 1 ? "y" : "ies");
    printf(rela ? "    Offset             Info             Type               Symbol's Value  Symbol's Name + Addend\n" :
                  "    Offset             Info             Type               Symbol's Value  Symbol's Name\n");
    const Elf64_Shdr *symtab = elffile_linked(f, sh);
    size_t nsyms = 0;
    const Elf64_Shdr *strtab = NULL;
    const Elf64_Sym *syms = symtab && symtab->sh_type != SHT_NULL ? elffile_symbols(f, symtab, &nsyms, &strtab) : NULL;
    struct elfdump_versions v;
    int dynamic = symtab && symtab->sh_type == SHT_DYNSYM;
    if (dynamic)
        elfdump_versions_init(f, symtab, &v);
    for (long i = 0; i < n; i++) {
        char buf[64];
        uint32_t type = ELF64_R_TYPE(r[i].info), symidx = ELF64_R_SYM(r[i].info);
        printf("%16.16llx  %16.16llx %-22s", u64(r[i].offset), u64(r[i].info),
               reloc_type_name(f->eh->e_machine, type, buf, sizeof buf));
        if (symidx != 0 && syms) {
            printf(" ");
            print_reloc_symbol(f, &v, syms, nsyms, strtab, dynamic, symidx);
        } else if (rela) {
            printf("                    ");
        }
        if (rela) {
            if (symidx != 0 && syms) {
                if (r[i].addend < 0)
                    printf(" - %llx", u64((uint64_t)-r[i].addend));
                else
                    printf(" + %llx", u64((uint64_t)r[i].addend));
            } else if (r[i].addend < 0) {
                printf("-%llx", u64((uint64_t)-r[i].addend));
            } else {
                printf("%llx", u64((uint64_t)r[i].addend));
            }
        }
        printf("\n");
    }
    free(r);
}

static void print_relocs(const struct elffile *f)
{
    int found = 0;
    for (unsigned i = 0; i < f->nsections; i++) {
        const Elf64_Shdr *s = &f->sh[i];
        if ((s->sh_type == SHT_RELA || s->sh_type == SHT_REL) && s->sh_size != 0) {
            print_reloc_section(f, s);
            found = 1;
        }
    }
    if (!found)
        printf("\nThere are no relocations in this file.\n");
}

/* ---- unwind and architecture information ---- */

static void print_unwind(const struct elffile *f)
{
    unsigned machine = f->eh->e_machine;
    if (machine == EM_X86_64) {
        printf("No processor specific unwind information to decode\n");
        return;
    }
    const char *name = elffile_machine_name(machine);
    char buf[40];
    if (!name) {
        snprintf(buf, sizeof buf, "<unknown>: 0x%x", machine);
        name = buf;
    }
    printf("\nThe decoding of unwind sections for machine type %s is not currently supported.\n", name);
}

/* ---- architecture specific information ---- */

/* read_uleb128 decodes an unsigned LEB128 number at *p. */
static uint64_t read_uleb128(const uint8_t **p, const uint8_t *end)
{
    uint64_t v = 0;
    unsigned shift = 0;
    while (*p < end) {
        uint8_t b = *(*p)++;
        if (shift < 64)
            v |= (uint64_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80))
            break;
    }
    return v;
}

static void print_aarch64_attributes(const struct elffile *f, const Elf64_Shdr *s)
{
    const uint8_t *data = elffile_section_data(f, s);
    if (!data || s->sh_size < 1 || data[0] != 'A')
        return;
    const uint8_t *end = data + s->sh_size, *p = data + 1;
    printf("Subsections:\n");
    while (end - p >= 4) {
        uint32_t len;
        memcpy(&len, p, 4);
        if (len < 4 || len > (uint64_t)(end - p))
            break;
        const uint8_t *sub_end = p + len, *q = p + 4;
        const char *vendor = (const char *)q;
        size_t vlen = strnlen(vendor, (size_t)(sub_end - q));
        q += vlen + 1;
        if (q + 2 > sub_end)
            break;
        unsigned scope = q[0], flags = q[1];
        q += 2;
        printf(" - Name:\t  %.*s\n", (int)vlen, vendor);
        printf("   Scope:\t  %s\n", scope == 1 ? "public" : "private");
        printf("   Length:\t  %u\n", len);
        printf("   Comprehension: %s\n", (flags & 1) ? "required" : "optional");
        printf("   Encoding:\t  %s\n", (flags & 2) ? "NTBS" : "ULEB128");
        printf("   Values:\n");
        int feature = strcmp(vendor, "aeabi_feature_and_bits") == 0;
        while (q < sub_end) {
            uint64_t tag = read_uleb128(&q, sub_end);
            uint64_t value = read_uleb128(&q, sub_end);
            static const char *const feature_tags[] = { "Tag_Feature_BTI", "Tag_Feature_PAC", "Tag_Feature_GCS" };
            if (feature && tag < 3)
                printf("    %s:\t%llu (0x%llx)\n", feature_tags[tag], u64(value), u64(value));
            else
                printf("    Tag_%llu:\t%llu (0x%llx)\n", u64(tag), u64(value), u64(value));
        }
        printf("\n");
        p = sub_end;
    }
}

static void print_arch_specific(const struct elffile *f)
{
    if (f->eh->e_machine != EM_AARCH64)
        return;
    for (unsigned i = 0; i < f->nsections; i++)
        if (f->sh[i].sh_type == SHT_AARCH64_ATTRIBUTES)
            print_aarch64_attributes(f, &f->sh[i]);
}

/* ---- version information ---- */

static void print_versym_section(const struct elffile *f, const Elf64_Shdr *s)
{
    const uint16_t *data = elffile_section_data(f, s);
    if (!data)
        return;
    size_t n = s->sh_size / 2;
    const Elf64_Shdr *symtab = elffile_linked(f, s);
    struct elfdump_versions v;
    if (symtab)
        elfdump_versions_init(f, symtab, &v);
    else
        memset(&v, 0, sizeof v);
    printf("\nVersion symbols section '%s' contains %zu entr%s:\n", elffile_section_name(f, s), n,
           n == 1 ? "y" : "ies");
    printf(" Addr: 0x%016llx  Offset: 0x%08llx  Link: %u (%s)\n", u64(s->sh_addr), u64(s->sh_offset), s->sh_link,
           symtab ? elffile_section_name(f, symtab) : "<corrupt>");
    for (size_t cnt = 0; cnt < n; cnt += 4) {
        printf("  %03zx:", cnt);
        for (size_t j = 0; j < 4 && cnt + j < n; j++) {
            unsigned ndx = data[cnt + j];
            int used;
            if (ndx == 0) {
                used = printf("   0 (*local*)");
            } else if (ndx == 1) {
                used = printf("   1 (*global*)");
            } else {
                int is_need;
                const char *name = elfdump_version_name(f, &v, ndx, &is_need);
                used = printf("%4x%c(%s)", ndx & 0x7fff, ndx & 0x8000 ? 'h' : ' ', name ? name : "*invalid*");
            }
            /* An entry fills 18 columns.  An entry of exactly 18 columns receives one blank. */
            int pad = 18 - used;
            printf("%*s", pad > 0 ? pad : (pad == 0 ? 1 : -pad), "");
        }
        printf("\n");
    }
}

/* print_version_offset writes an offset as %#06x does: 000000 for zero, else 0x and four digits. */
static void print_version_offset(uint64_t off)
{
    if (off == 0)
        printf("000000");
    else
        printf("0x%04llx", u64(off));
}

static void print_verdef_section(const struct elffile *f, const Elf64_Shdr *s)
{
    const uint8_t *data = elffile_section_data(f, s);
    if (!data)
        return;
    const Elf64_Shdr *strtab = elffile_linked(f, s);
    printf("\nVersion definition section '%s' contains %u entr%s:\n", elffile_section_name(f, s), s->sh_info,
           s->sh_info == 1 ? "y" : "ies");
    printf(" Addr: 0x%016llx  Offset: 0x%08llx  Link: %u (%s)\n", u64(s->sh_addr), u64(s->sh_offset), s->sh_link,
           strtab ? elffile_section_name(f, strtab) : "<corrupt>");
    uint64_t off = 0;
    for (unsigned i = 0; i < s->sh_info; i++) {
        if (off > s->sh_size || s->sh_size - off < sizeof(Elf64_Verdef))
            break;
        Elf64_Verdef d;
        memcpy(&d, data + off, sizeof d);
        printf("  ");
        print_version_offset(off);
        printf(": Rev: %u  Flags: ", d.vd_version);
        if (d.vd_flags == 0) {
            printf("none");
        } else {
            int first = 1;
            if (d.vd_flags & VER_FLG_BASE) {
                printf("BASE");
                first = 0;
            }
            if (d.vd_flags & VER_FLG_WEAK) {
                printf("%sWEAK", first ? "" : " | ");
                first = 0;
            }
            if (d.vd_flags & ~(unsigned)(VER_FLG_BASE | VER_FLG_WEAK))
                printf("%s<unknown>", first ? "" : " | ");
        }
        printf("  Index: %u  Cnt: %u  ", d.vd_ndx, d.vd_cnt);
        uint64_t aoff = off + d.vd_aux;
        if (aoff <= s->sh_size && s->sh_size - aoff >= sizeof(Elf64_Verdaux) && d.vd_cnt > 0) {
            Elf64_Verdaux a;
            memcpy(&a, data + aoff, sizeof a);
            const char *name = elffile_string(f, strtab, a.vda_name);
            printf("Name: %s\n", name ? name : "<corrupt>");
            for (unsigned k = 1; k < d.vd_cnt; k++) {
                if (a.vda_next == 0)
                    break;
                aoff += a.vda_next;
                if (aoff > s->sh_size || s->sh_size - aoff < sizeof(Elf64_Verdaux))
                    break;
                memcpy(&a, data + aoff, sizeof a);
                name = elffile_string(f, strtab, a.vda_name);
                printf("  ");
                print_version_offset(aoff);
                printf(": Parent %u: %s\n", k, name ? name : "<corrupt>");
            }
        } else {
            printf("Name: <corrupt>\n");
        }
        if (d.vd_next == 0)
            break;
        off += d.vd_next;
    }
}

static void print_verneed_section(const struct elffile *f, const Elf64_Shdr *s)
{
    const uint8_t *data = elffile_section_data(f, s);
    if (!data)
        return;
    const Elf64_Shdr *strtab = elffile_linked(f, s);
    printf("\nVersion needs section '%s' contains %u entr%s:\n", elffile_section_name(f, s), s->sh_info,
           s->sh_info == 1 ? "y" : "ies");
    printf(" Addr: 0x%016llx  Offset: 0x%08llx  Link: %u (%s)\n", u64(s->sh_addr), u64(s->sh_offset), s->sh_link,
           strtab ? elffile_section_name(f, strtab) : "<corrupt>");
    uint64_t off = 0;
    for (unsigned i = 0; i < s->sh_info; i++) {
        if (off > s->sh_size || s->sh_size - off < sizeof(Elf64_Verneed))
            break;
        Elf64_Verneed n;
        memcpy(&n, data + off, sizeof n);
        const char *file = elffile_string(f, strtab, n.vn_file);
        printf("  ");
        print_version_offset(off);
        printf(": Version: %u  File: %s  Cnt: %u\n", n.vn_version, file ? file : "<corrupt>", n.vn_cnt);
        uint64_t aoff = off + n.vn_aux;
        for (unsigned k = 0; k < n.vn_cnt; k++) {
            if (aoff > s->sh_size || s->sh_size - aoff < sizeof(Elf64_Vernaux))
                break;
            Elf64_Vernaux a;
            memcpy(&a, data + aoff, sizeof a);
            const char *name = elffile_string(f, strtab, a.vna_name);
            printf("  ");
            print_version_offset(aoff);
            printf(":   Name: %s  Flags: %s  Version: %u\n", name ? name : "<corrupt>",
                   (a.vna_flags & VER_FLG_WEAK) ? "WEAK" : "none", a.vna_other);
            if (a.vna_next == 0)
                break;
            aoff += a.vna_next;
        }
        if (n.vn_next == 0)
            break;
        off += n.vn_next;
    }
}

static void print_versions(const struct elffile *f)
{
    int found = 0;
    for (unsigned i = 0; i < f->nsections; i++) {
        const Elf64_Shdr *s = &f->sh[i];
        if (s->sh_type == SHT_GNU_versym) {
            print_versym_section(f, s);
            found = 1;
        } else if (s->sh_type == SHT_GNU_verdef) {
            print_verdef_section(f, s);
            found = 1;
        } else if (s->sh_type == SHT_GNU_verneed) {
            print_verneed_section(f, s);
            found = 1;
        }
    }
    if (!found)
        printf("\nNo version information found in this file.\n");
}

/* ---- section contents ---- */

/* print_relocation_note prints the note about the relocations that a dump does not apply. */
static void print_relocation_note(const struct elffile *f, unsigned index)
{
    for (unsigned i = 0; i < f->nsections; i++) {
        const Elf64_Shdr *r = &f->sh[i];
        if ((r->sh_type == SHT_RELA || r->sh_type == SHT_REL) && r->sh_info == index && r->sh_size != 0 &&
            r->sh_link < f->nsections) {
            printf(" NOTE: This section has relocations against it, but these have NOT been applied to this dump.\n");
            return;
        }
    }
}

static void dump_hex(const struct elffile *f, unsigned index)
{
    const Elf64_Shdr *s = &f->sh[index];
    const char *name = elffile_section_name(f, s);
    const uint8_t *data = elffile_section_data(f, s);
    if (s->sh_type == SHT_NOBITS || s->sh_size == 0 || !data) {
        printf("Section '%s' has no data to dump.\n", name);
        return;
    }
    printf("\nHex dump of section '%s':\n", name);
    print_relocation_note(f, index);
    uint64_t addr = s->sh_addr;
    for (uint64_t pos = 0; pos < s->sh_size; pos += 16, addr += 16) {
        size_t n = s->sh_size - pos < 16 ? (size_t)(s->sh_size - pos) : 16;
        printf("  0x%8.8llx ", u64(addr));
        for (size_t j = 0; j < 16; j++) {
            if (j < n)
                printf("%2.2x", data[pos + j]);
            else
                printf("  ");
            if ((j & 3) == 3)
                printf(" ");
        }
        for (size_t j = 0; j < n; j++)
            putchar(data[pos + j] >= ' ' && data[pos + j] < 0x7f ? data[pos + j] : '.');
        putchar('\n');
    }
    putchar('\n');
}

/* utf8_length returns the length of the valid UTF-8 sequence that starts at p, or 1. */
static size_t utf8_length(const uint8_t *p, size_t avail)
{
    size_t need;
    if (p[0] >= 0xc2 && p[0] <= 0xdf)
        need = 2;
    else if (p[0] >= 0xe0 && p[0] <= 0xef)
        need = 3;
    else if (p[0] >= 0xf0 && p[0] <= 0xf4)
        need = 4;
    else
        return 1;
    if (avail < need)
        return 1;
    for (size_t i = 1; i < need; i++)
        if ((p[i] & 0xc0) != 0x80)
            return 1;
    if ((p[0] == 0xe0 && p[1] < 0xa0) || (p[0] == 0xed && p[1] > 0x9f) || (p[0] == 0xf0 && p[1] < 0x90) ||
        (p[0] == 0xf4 && p[1] > 0x8f))
        return 1;
    return need;
}

/* locale_is_utf8 reports whether the character set of the locale is UTF-8. */
static int locale_is_utf8(void)
{
    static const char *const names[] = { "LC_ALL", "LC_CTYPE", "LANG" };
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        const char *v = getenv(names[i]);
        if (!v || !*v)
            continue;
        for (const char *p = v; *p; p++)
            if ((p[0] == 'U' || p[0] == 'u') && (p[1] == 'T' || p[1] == 't') && (p[2] == 'F' || p[2] == 'f') &&
                (p[3] == '-' ? p[4] == '8' : p[3] == '8'))
                return 1;
        return 0;
    }
    return 0;
}

/* print_string_bytes writes a string as readelf does: control characters appear as ^ and a letter,
 * a new line as \n, and a UTF-8 sequence as its first byte. */
static void print_string_bytes(const uint8_t *s, size_t len)
{
    static int utf8 = -1;
    if (utf8 < 0)
        utf8 = locale_is_utf8();
    for (size_t i = 0; i < len; i++) {
        uint8_t c = s[i];
        if (c == '\n') {
            fputs("\\n", stdout);
        } else if (c < ' ' || c == 0x7f) {
            putchar('^');
            putchar((uint8_t)(c + 0x40));
        } else if (c >= 0x80 && utf8) {
            putchar(c);
            i += utf8_length(s + i, len - i) - 1;
        } else {
            putchar(c);
        }
    }
}

static void dump_strings(const struct elffile *f, unsigned index)
{
    const Elf64_Shdr *s = &f->sh[index];
    const char *name = elffile_section_name(f, s);
    const uint8_t *data = elffile_section_data(f, s);
    if (s->sh_type == SHT_NOBITS || s->sh_size == 0 || !data) {
        printf("Section '%s' has no data to dump.\n", name);
        return;
    }
    printf("\nString dump of section '%s':\n", name);
    print_relocation_note(f, index);
    /* A string ends at a NUL byte or after a new line.  The label of a string that follows a new line is
     * blank, unless a NUL byte follows the new line. */
    int shown = 0, blank_label = 0;
    uint64_t pos = 0, size = s->sh_size;
    while (pos < size) {
        while (pos < size && !(data[pos] >= ' ' && data[pos] < 0x7f))
            pos++;
        if (pos >= size)
            break;
        uint64_t end = pos;
        while (end < size && data[end] != '\0' && data[end] != '\n')
            end++;
        if (blank_label)
            printf("%12s", "");
        else
            printf("  [%6llx]  ", u64(pos));
        print_string_bytes(data + pos, (size_t)(end - pos));
        int newline = end < size && data[end] == '\n';
        if (newline) {
            fputs("\\n", stdout);
            end++;
        }
        putchar('\n');
        shown = 1;
        blank_label = newline && end < size && data[end] != '\0';
        pos = end;
    }
    if (!shown)
        printf("  No strings found in this section.");
    putchar('\n');
}

static void print_section_contents(const struct elffile *f)
{
    for (size_t r = 0; r < nrequests; r++) {
        int matched = 0;
        for (unsigned i = 0; i < f->nsections; i++)
            if (elfdump_section_matches(f, i, requests[r].spec))
                matched = 1;
        if (!matched) {
            bu_error(NULL, "Warning: Section '%s' was not dumped because it does not exist", requests[r].spec);
        }
    }
    for (unsigned i = 0; i < f->nsections; i++) {
        int hex = 0, str = 0;
        for (size_t r = 0; r < nrequests; r++)
            if (elfdump_section_matches(f, i, requests[r].spec)) {
                if (requests[r].hex)
                    hex = 1;
                else
                    str = 1;
            }
        if (hex)
            dump_hex(f, i);
        if (str)
            dump_strings(f, i);
    }
}

/* ---- global offset table ---- */

/* got_entry_reloc looks for a dynamic relocation against the address addr.  On success it writes the type,
 * the symbol and the addend to text and returns 1. */
static int got_entry_reloc(const struct elffile *f, uint64_t addr, char *text, size_t len)
{
    for (unsigned si = 0; si < f->nsections; si++) {
        const Elf64_Shdr *rs = &f->sh[si];
        if (rs->sh_type != SHT_RELA && rs->sh_type != SHT_REL)
            continue;
        const Elf64_Shdr *symtab = elffile_linked(f, rs);
        if (!symtab || symtab->sh_type != SHT_DYNSYM)
            continue;
        struct elfdump_reloc *r;
        long nr = elfdump_read_relocs(f, rs, &r);
        for (long i = 0; i < nr; i++) {
            if (r[i].offset != addr)
                continue;
            char buf[64];
            uint32_t type = ELF64_R_TYPE(r[i].info), symidx = ELF64_R_SYM(r[i].info);
            int n = snprintf(text, len, "%-22s ", reloc_type_name(f->eh->e_machine, type, buf, sizeof buf));
            size_t nsyms;
            const Elf64_Shdr *strtab;
            const Elf64_Sym *syms = elffile_symbols(f, symtab, &nsyms, &strtab);
            if (symidx != 0 && syms && symidx < nsyms) {
                struct elfdump_versions v;
                elfdump_versions_init(f, symtab, &v);
                snprintf(text + n, len - n, "%s %c %llx", elfdump_format_symbol_name(f, &v, strtab, &syms[symidx], symidx, 0),
                         r[i].addend < 0 ? '-' : '+',
                         u64(r[i].addend < 0 ? (uint64_t)-r[i].addend : (uint64_t)r[i].addend));
            } else {
                snprintf(text + n, len - n, "%llx", u64((uint64_t)r[i].addend));
            }
            free(r);
            return 1;
        }
        free(r);
    }
    return 0;
}

static void print_got(const struct elffile *f)
{
    int has_relocs = 0, found = 0;
    for (unsigned i = 0; i < f->nsections; i++)
        if ((f->sh[i].sh_type == SHT_RELA || f->sh[i].sh_type == SHT_REL) && f->sh[i].sh_size != 0)
            has_relocs = 1;
    if (!has_relocs)
        return;
    for (unsigned i = 0; i < f->nsections; i++) {
        const Elf64_Shdr *got = &f->sh[i];
        const char *name = elffile_section_name(f, got);
        if (got->sh_type != SHT_PROGBITS || (strcmp(name, ".got") != 0 && strcmp(name, ".got.plt") != 0))
            continue;
        const uint8_t *data = elffile_section_data(f, got);
        if (!data || got->sh_size < 8)
            continue;
        found = 1;
        size_t n = got->sh_size / 8;
        printf("\nGlobal Offset Table '%s' contains %zu entr%s:\n", name, n, n == 1 ? "y" : "ies");
        printf("   Index:      Address          Reloc             Sym. Name + Addend/Value\n");
        for (size_t k = 0; k < n; k++) {
            char text[400];
            printf("%8zu: %16.16llx ", k, u64(got->sh_addr + k * 8));
            if (got_entry_reloc(f, got->sh_addr + k * 8, text, sizeof text)) {
                printf("%s\n", text);
            } else {
                /* readelf reads the value at four bytes per entry. */
                uint32_t value;
                memcpy(&value, data + k * 4, 4);
                printf("%-22s %x\n", "", value);
            }
        }
    }
    if (!found)
        printf("\nThere is no GOT section in this file.\n");
}

/* ---- notes ---- */

static void print_hex_bytes(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        printf("%02x", p[i]);
}

/* print_property_flags writes the names of the set bits of value after a label, separated by commas. */
static void print_property_flags(const char *label, uint32_t value, const char *const *names, unsigned nnames,
                                 int *first)
{
    const char *sep = *first ? "" : ", ";
    printf("%s%s", sep, label);
    *first = 0;
    if (value == 0) {
        printf("<None>");
        return;
    }
    int local_first = 1;
    for (unsigned i = 0; i < 32; i++) {
        if (!(value & (1u << i)))
            continue;
        printf("%s", local_first ? "" : ", ");
        local_first = 0;
        if (i < nnames && names[i])
            printf("%s", names[i]);
        else
            printf("<unknown: %x>", 1u << i);
    }
}

static void print_note_gnu_property(const struct elffile *f, const uint8_t *desc, size_t size)
{
    static const char *const x86_features[] = { "IBT", "SHSTK", "LAM_U48", "LAM_U57" };
    static const char *const x86_isa[] = { "x86-64-baseline", "x86-64-v2", "x86-64-v3", "x86-64-v4" };
    static const char *const x86_features_2[] = { "x86", "x87", "MMX", "XMM", "YMM", "ZMM", "FXSR", "XSAVE",
                                                  "XSAVEOPT", "XSAVEC", "TMM", "MASK" };
    static const char *const aarch64_features[] = { "BTI", "PAC", "GCS" };
    unsigned machine = f->eh->e_machine;
    size_t pos = 0;
    int first = 1;
    printf("      Properties: ");
    while (pos < size) {
        if (size - pos < 8) {
            printf("<corrupt descsz: 0x%zx>", size);
            break;
        }
        uint32_t type, datasz, value = 0;
        memcpy(&type, desc + pos, 4);
        memcpy(&datasz, desc + pos + 4, 4);
        pos += 8;
        if (datasz > size - pos) {
            printf("<corrupt length: 0x%x>", datasz);
            break;
        }
        if (datasz >= 4)
            memcpy(&value, desc + pos, 4);
        if (machine == EM_X86_64 && type == 0xc0000002)
            print_property_flags("x86 feature: ", value, x86_features, 4, &first);
        else if (machine == EM_X86_64 && type == 0xc0008002)
            print_property_flags("x86 ISA needed: ", value, x86_isa, 4, &first);
        else if (machine == EM_X86_64 && type == 0xc0010002)
            print_property_flags("x86 ISA used: ", value, x86_isa, 4, &first);
        else if (machine == EM_X86_64 && type == 0xc0008001)
            print_property_flags("x86 feature needed: ", value, x86_features_2, 12, &first);
        else if (machine == EM_X86_64 && type == 0xc0010001)
            print_property_flags("x86 feature used: ", value, x86_features_2, 12, &first);
        else if (machine == EM_AARCH64 && type == 0xc0000000)
            print_property_flags("AArch64 feature: ", value, aarch64_features, 3, &first);
        else {
            printf("%s<unknown type %x data: ", first ? "" : ", ", type);
            first = 0;
            for (uint32_t i = 0; i < datasz; i++)
                printf("%s%02x", i ? " " : "", desc[pos + i]);
            printf(">");
        }
        pos += (datasz + 7) & ~(size_t)7;
    }
    printf("\n");
}

static void print_note(const struct elffile *f, const char *owner, uint32_t type, const uint8_t *desc, size_t descsz)
{
    int gnu = strcmp(owner, "GNU") == 0;
    const char *desc_name = NULL;
    char buf[48];
    if (gnu) {
        switch (type) {
        case NT_GNU_ABI_TAG:        desc_name = "NT_GNU_ABI_TAG (ABI version tag)"; break;
        case NT_GNU_HWCAP:          desc_name = "NT_GNU_HWCAP (DSO-supplied software HWCAP info)"; break;
        case NT_GNU_BUILD_ID:       desc_name = "NT_GNU_BUILD_ID (unique build ID bitstring)"; break;
        case NT_GNU_GOLD_VERSION:   desc_name = "NT_GNU_GOLD_VERSION (gold version)"; break;
        case NT_GNU_PROPERTY_TYPE_0: desc_name = "NT_GNU_PROPERTY_TYPE_0"; break;
        default: break;
        }
    }
    if (!desc_name) {
        snprintf(buf, sizeof buf, "Unknown note type: (0x%08x)", type);
        desc_name = buf;
    }
    printf("  %-20s 0x%08zx\t%s\t", owner, descsz, desc_name);
    if (gnu && type == NT_GNU_BUILD_ID) {
        printf("    Build ID: ");
        print_hex_bytes(desc, descsz);
        printf("\n");
    } else if (gnu && type == NT_GNU_ABI_TAG && descsz >= 16) {
        uint32_t w[4];
        memcpy(w, desc, sizeof w);
        const char *os;
        switch (w[0]) {
        case 0:  os = "Linux"; break;
        case 1:  os = "Hurd"; break;
        case 2:  os = "Solaris"; break;
        case 3:  os = "FreeBSD"; break;
        case 4:  os = "NetBSD"; break;
        case 5:  os = "Syllable"; break;
        case 6:  os = "NaCl"; break;
        default: os = "Unknown"; break;
        }
        printf("    OS: %s, ABI: %u.%u.%u\n", os, w[1], w[2], w[3]);
    } else if (gnu && type == NT_GNU_GOLD_VERSION) {
        printf("    Version: %.*s\n", (int)descsz, desc);
    } else if (gnu && type == NT_GNU_HWCAP && descsz >= 8) {
        uint32_t w[2];
        memcpy(w, desc, sizeof w);
        printf("      Hardware Capabilities: num entries: %u, enabled mask: %x\n", w[0], w[1]);
    } else if (gnu && type == NT_GNU_PROPERTY_TYPE_0) {
        print_note_gnu_property(f, desc, descsz);
    } else {
        if (descsz > 0) {
            printf("   description data: ");
            for (size_t i = 0; i < descsz; i++)
                printf("%02x ", desc[i]);
        }
        printf("\n");
    }
}

static void print_notes_in(const struct elffile *f, const uint8_t *data, size_t size)
{
    printf("  %-20s %s \t%s\n", "Owner", "Data size", "Description");
    size_t pos = 0;
    while (size - pos >= sizeof(Elf64_Nhdr)) {
        Elf64_Nhdr h;
        memcpy(&h, data + pos, sizeof h);
        pos += sizeof h;
        size_t namesz = h.n_namesz, descsz = h.n_descsz;
        size_t name_len = (namesz + 3) & ~(size_t)3, desc_len = (descsz + 3) & ~(size_t)3;
        if (name_len > size - pos || desc_len > size - pos - name_len) {
            bu_error(NULL, "Warning: note with invalid namesz and/or descsz found at offset 0x%zx", pos - sizeof h);
            return;
        }
        char owner[64];
        size_t cp = namesz;
        if (cp > 0 && data[pos + cp - 1] == '\0')
            cp--;
        if (cp >= sizeof owner)
            cp = sizeof owner - 1;
        memcpy(owner, data + pos, cp);
        owner[cp] = '\0';
        print_note(f, owner, h.n_type, data + pos + name_len, descsz);
        pos += name_len + desc_len;
    }
}

static void print_notes(const struct elffile *f)
{
    int found = 0;
    for (unsigned i = 0; i < f->nsections; i++) {
        const Elf64_Shdr *s = &f->sh[i];
        if (s->sh_type != SHT_NOTE)
            continue;
        const uint8_t *data = elffile_section_data(f, s);
        if (!data)
            continue;
        found = 1;
        printf("\nDisplaying notes found in: %s\n", elffile_section_name(f, s));
        print_notes_in(f, data, s->sh_size);
    }
    if (found)
        return;
    for (unsigned i = 0; i < f->nsegments; i++) {
        const Elf64_Phdr *p = &f->ph[i];
        if (p->p_type != PT_NOTE || p->p_offset > f->size || p->p_filesz > f->size - p->p_offset)
            continue;
        printf("\nDisplaying notes found at file offset 0x%08llx with length 0x%08llx:\n", u64(p->p_offset),
               u64(p->p_filesz));
        print_notes_in(f, f->data + p->p_offset, p->p_filesz);
    }
}

/* ---- driver ---- */

static int process_object(const char *path, const char *member, const struct elffile *f, void *arg)
{
    (void)arg;
    if (member)
        printf("\nFile: %s(%s)\n", path, member);
    else if (show_names)
        printf("\nFile: %s\n", path);
    if (opt.header)
        print_file_header(f);
    if (opt.sections)
        print_section_headers(f);
    if (opt.groups)
        print_section_groups(f);
    if (opt.segments)
        print_program_headers(f);
    if (opt.dynamic)
        print_dynamic_section(f);
    if (opt.relocs)
        print_relocs(f);
    if (opt.unwind)
        print_unwind(f);
    if (opt.syms || opt.dyn_syms)
        print_symbols(f);
    if (opt.histogram)
        print_histograms(f);
    if (opt.version)
        print_versions(f);
    if (nrequests)
        print_section_contents(f);
    if (opt.got)
        print_got(f);
    if (opt.notes)
        print_notes(f);
    if (opt.arch)
        print_arch_specific(f);
    return 0;
}

static void announce_bad_member(const char *path, const char *member, void *arg)
{
    (void)arg;
    printf("\nFile: %s(%s)\n", path, member);
}

static void usage(void)
{
    fprintf(stderr, "Usage: readelf <option(s)> elf-file(s)\n"
                    " Display information about the contents of ELF format files\n"
                    " Options are:\n"
                    "  -a --all               Equivalent to: -h -l -S -s -r -d -V -A -I --got-contents\n"
                    "  -h --file-header       Display the ELF file header\n"
                    "  -l --program-headers   Display the program headers\n"
                    "  -S --section-headers   Display the sections' header\n"
                    "  -g --section-groups    Display the section groups\n"
                    "  -e --headers           Equivalent to: -h -l -S\n"
                    "  -s --syms              Display the symbol table\n"
                    "     --dyn-syms          Display the dynamic symbol table\n"
                    "  -n --notes             Display the contents of note sections (if present)\n"
                    "  -r --relocs            Display the relocations (if present)\n"
                    "  -u --unwind            Display the unwind info (if present)\n"
                    "  -d --dynamic           Display the dynamic section (if present)\n"
                    "  -V --version-info      Display the version sections (if present)\n"
                    "  -A --arch-specific     Display architecture specific information (if any)\n"
                    "  -x --hex-dump=<number|name>\n"
                    "                         Dump the contents of section <number|name> as bytes\n"
                    "  -p --string-dump=<number|name>\n"
                    "                         Dump the contents of section <number|name> as strings\n"
                    "  -I --histogram         Display histogram of bucket list lengths\n"
                    "     --got-contents      Display GOT section contents\n"
                    "  -W --wide              Allow output width to exceed 80 characters\n"
                    "  -H --help              Display this information\n"
                    "  -v --version           Display the version number of readelf\n");
}

enum {
    OPT_DYN_SYMS = 256,
    OPT_GOT,
};

int main(int argc, char **argv)
{
    bu_program = "readelf";
    static const struct option longopts[] = {
        { "all", no_argument, NULL, 'a' },
        { "file-header", no_argument, NULL, 'h' },
        { "program-headers", no_argument, NULL, 'l' },
        { "segments", no_argument, NULL, 'l' },
        { "section-headers", no_argument, NULL, 'S' },
        { "sections", no_argument, NULL, 'S' },
        { "section-groups", no_argument, NULL, 'g' },
        { "headers", no_argument, NULL, 'e' },
        { "syms", no_argument, NULL, 's' },
        { "symbols", no_argument, NULL, 's' },
        { "dyn-syms", no_argument, NULL, OPT_DYN_SYMS },
        { "notes", no_argument, NULL, 'n' },
        { "relocs", no_argument, NULL, 'r' },
        { "unwind", no_argument, NULL, 'u' },
        { "dynamic", no_argument, NULL, 'd' },
        { "version-info", no_argument, NULL, 'V' },
        { "arch-specific", no_argument, NULL, 'A' },
        { "hex-dump", required_argument, NULL, 'x' },
        { "string-dump", required_argument, NULL, 'p' },
        { "histogram", no_argument, NULL, 'I' },
        { "got-contents", no_argument, NULL, OPT_GOT },
        { "wide", no_argument, NULL, 'W' },
        { "help", no_argument, NULL, 'H' },
        { "version", no_argument, NULL, 'v' },
        { NULL, 0, NULL, 0 },
    };
    int c;
    while ((c = getopt_long(argc, argv, "ahlSgesnrudVAx:p:IWHv", longopts, NULL)) != -1) {
        switch (c) {
        case 'a':
            opt.header = opt.segments = opt.sections = opt.groups = opt.syms = opt.dyn_syms = 1;
            opt.relocs = opt.unwind = opt.dynamic = opt.version = opt.arch = opt.histogram = 1;
            opt.notes = opt.got = 1;
            break;
        case 'h': opt.header = 1; break;
        case 'l': opt.segments = 1; break;
        case 'S': opt.sections = 1; break;
        case 'g': opt.groups = 1; break;
        case 'e': opt.header = opt.segments = opt.sections = 1; break;
        case 's': opt.syms = opt.dyn_syms = 1; break;
        case OPT_DYN_SYMS: opt.dyn_syms = 1; break;
        case 'n': opt.notes = 1; break;
        case 'r': opt.relocs = 1; break;
        case 'u': opt.unwind = 1; break;
        case 'd': opt.dynamic = 1; break;
        case 'V': opt.version = 1; break;
        case 'A': opt.arch = 1; break;
        case 'I': opt.histogram = 1; break;
        case OPT_GOT: opt.got = 1; break;
        case 'W': break;
        case 'x':
        case 'p':
            requests = bu_realloc(requests, (nrequests + 1) * sizeof *requests);
            requests[nrequests].spec = optarg;
            requests[nrequests].hex = c == 'x';
            nrequests++;
            break;
        case 'v':
            printf("readelf (minios binutils) 0.5\n");
            return 0;
        case 'H':
            usage();
            return 0;
        default:
            usage();
            return 1;
        }
    }
    if (!(opt.header || opt.segments || opt.sections || opt.groups || opt.syms || opt.dyn_syms || opt.notes ||
          opt.relocs || opt.unwind || opt.dynamic || opt.version || opt.arch || opt.histogram || opt.got ||
          nrequests)) {
        bu_error(NULL, "Warning: Nothing to do.");
        usage();
        return 1;
    }
    if ((argc - optind) == 0) {
        bu_error(NULL, "Warning: Nothing to do.");
        usage();
        return 1;
    }
    show_names = (argc - optind) > 1;
    int status = 0;
    for (int i = 0; i < (argc - optind); i++)
        if (bu_for_each_object(argv[optind + i], NULL, process_object, announce_bad_member, NULL))
            status = 1;
    return status;
}
