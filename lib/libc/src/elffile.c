/* The ELF64 reader of <minios/elffile.h>. */
#include <minios/elffile.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* range_ok reports whether count entries of size bytes at off lie in the file. */
static int range_ok(const struct elffile *f, uint64_t off, uint64_t count, uint64_t size)
{
    if (off > f->size)
        return 0;
    if (size && count > (f->size - off) / size)
        return 0;
    return 1;
}

int elffile_open(struct elffile *f, const void *data, size_t size)
{
    memset(f, 0, sizeof *f);
    f->data = data;
    f->size = size;
    const Elf64_Ehdr *eh = data;
    if (size < sizeof *eh || memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_ident[EI_CLASS] != ELFCLASS64 ||
        eh->e_ident[EI_DATA] != ELFDATA2LSB)
        return -ENOEXEC;
    f->eh = eh;
    if (eh->e_shoff && eh->e_shnum) {
        if (eh->e_shentsize != sizeof(Elf64_Shdr) || !range_ok(f, eh->e_shoff, eh->e_shnum, sizeof(Elf64_Shdr)))
            return -EINVAL;
        f->sh = (const Elf64_Shdr *)(f->data + eh->e_shoff);
        f->nsections = eh->e_shnum;
        if (eh->e_shstrndx != SHN_UNDEF && eh->e_shstrndx < f->nsections &&
            f->sh[eh->e_shstrndx].sh_type == SHT_STRTAB &&
            range_ok(f, f->sh[eh->e_shstrndx].sh_offset, f->sh[eh->e_shstrndx].sh_size, 1))
            f->shstrtab = &f->sh[eh->e_shstrndx];
    }
    if (eh->e_phoff && eh->e_phnum) {
        if (eh->e_phentsize != sizeof(Elf64_Phdr) || !range_ok(f, eh->e_phoff, eh->e_phnum, sizeof(Elf64_Phdr)))
            return -EINVAL;
        f->ph = (const Elf64_Phdr *)(f->data + eh->e_phoff);
        f->nsegments = eh->e_phnum;
    }
    return 0;
}

int elffile_load(struct elffile *f, const char *path, void **buf)
{
    *buf = NULL;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -errno;
    struct stat st;
    if (fstat(fd, &st) < 0) {
        int err = errno;
        close(fd);
        return -err;
    }
    size_t size = (size_t)st.st_size;
    uint8_t *data = malloc(size ? size : 1);
    if (!data) {
        close(fd);
        return -ENOMEM;
    }
    size_t done = 0;
    while (done < size) {
        ssize_t r = read(fd, data + done, size - done);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0) {
            int err = r < 0 ? errno : EIO;
            free(data);
            close(fd);
            return -err;
        }
        done += (size_t)r;
    }
    close(fd);
    int r = elffile_open(f, data, size);
    if (r < 0) {
        free(data);
        return r;
    }
    *buf = data;
    return 0;
}

const Elf64_Shdr *elffile_section(const struct elffile *f, unsigned index)
{
    return f->sh && index < f->nsections ? &f->sh[index] : NULL;
}

unsigned elffile_section_index(const struct elffile *f, const Elf64_Shdr *s)
{
    return (unsigned)(s - f->sh);
}

const char *elffile_section_name(const struct elffile *f, const Elf64_Shdr *s)
{
    const char *name = f->shstrtab ? elffile_string(f, f->shstrtab, s->sh_name) : NULL;
    return name ? name : "";
}

const Elf64_Shdr *elffile_find_section(const struct elffile *f, const char *name)
{
    for (unsigned i = 0; i < f->nsections; i++)
        if (strcmp(elffile_section_name(f, &f->sh[i]), name) == 0)
            return &f->sh[i];
    return NULL;
}

const Elf64_Shdr *elffile_find_type(const struct elffile *f, uint32_t type)
{
    for (unsigned i = 0; i < f->nsections; i++)
        if (f->sh[i].sh_type == type)
            return &f->sh[i];
    return NULL;
}

const void *elffile_section_data(const struct elffile *f, const Elf64_Shdr *s)
{
    if (s->sh_type == SHT_NOBITS || !range_ok(f, s->sh_offset, s->sh_size, 1))
        return NULL;
    return f->data + s->sh_offset;
}

const Elf64_Shdr *elffile_linked(const struct elffile *f, const Elf64_Shdr *s)
{
    return elffile_section(f, s->sh_link);
}

const char *elffile_string(const struct elffile *f, const Elf64_Shdr *strtab, uint64_t off)
{
    if (!strtab || strtab->sh_type == SHT_NOBITS || !range_ok(f, strtab->sh_offset, strtab->sh_size, 1) ||
        off >= strtab->sh_size)
        return NULL;
    const char *s = (const char *)f->data + strtab->sh_offset + off;
    return memchr(s, '\0', strtab->sh_size - off) ? s : NULL;
}

const Elf64_Sym *elffile_symbols(const struct elffile *f, const Elf64_Shdr *symtab, size_t *count,
                                 const Elf64_Shdr **strtab)
{
    *count = 0;
    *strtab = NULL;
    if (!symtab || (symtab->sh_type != SHT_SYMTAB && symtab->sh_type != SHT_DYNSYM) ||
        !range_ok(f, symtab->sh_offset, symtab->sh_size, 1))
        return NULL;
    const Elf64_Shdr *str = elffile_linked(f, symtab);
    if (!str || str->sh_type != SHT_STRTAB || !range_ok(f, str->sh_offset, str->sh_size, 1))
        return NULL;
    *count = symtab->sh_size / sizeof(Elf64_Sym);
    *strtab = str;
    return (const Elf64_Sym *)(f->data + symtab->sh_offset);
}

const char *elffile_symbol_name(const struct elffile *f, const Elf64_Shdr *strtab, const Elf64_Sym *sym)
{
    if (sym->st_name == 0 && ELF64_ST_TYPE(sym->st_info) == STT_SECTION) {
        const Elf64_Shdr *s = elffile_section(f, sym->st_shndx);
        return s ? elffile_section_name(f, s) : "";
    }
    const char *name = elffile_string(f, strtab, sym->st_name);
    return name ? name : "";
}

const Elf64_Dyn *elffile_dynamic(const struct elffile *f, size_t *count, const Elf64_Shdr **strtab)
{
    *count = 0;
    *strtab = NULL;
    const Elf64_Shdr *s = elffile_find_type(f, SHT_DYNAMIC);
    if (!s || !range_ok(f, s->sh_offset, s->sh_size, 1))
        return NULL;
    const Elf64_Dyn *d = (const Elf64_Dyn *)(f->data + s->sh_offset);
    size_t n = s->sh_size / sizeof *d, i = 0;
    while (i < n && d[i].d_tag != DT_NULL)
        i++;
    *count = i;
    *strtab = elffile_linked(f, s);
    if (*strtab && (*strtab)->sh_type != SHT_STRTAB)
        *strtab = NULL;
    return d;
}

/* ---- names ---- */

struct name_entry {
    int64_t value;
    const char *name;
};

static const char *lookup(const struct name_entry *table, size_t n, int64_t value)
{
    for (size_t i = 0; i < n; i++)
        if (table[i].value == value)
            return table[i].name;
    return NULL;
}

#define LOOKUP(table, value) lookup(table, sizeof table / sizeof table[0], value)

static const struct name_entry types[] = {
    { ET_NONE, "NONE (None)" },
    { ET_REL, "REL (Relocatable file)" },
    { ET_EXEC, "EXEC (Executable file)" },
    { ET_DYN, "DYN (Shared object file)" },
    { ET_CORE, "CORE (Core file)" },
};

static const struct name_entry machines[] = {
    { EM_NONE, "None" },
    { EM_386, "Intel 80386" },
    { EM_ARM, "ARM" },
    { EM_X86_64, "Advanced Micro Devices X86-64" },
    { EM_AARCH64, "AArch64" },
    { EM_RISCV, "RISC-V" },
};

static const struct name_entry osabis[] = {
    { ELFOSABI_NONE, "UNIX - System V" },
    { ELFOSABI_HPUX, "UNIX - HP-UX" },
    { ELFOSABI_NETBSD, "UNIX - NetBSD" },
    { ELFOSABI_GNU, "UNIX - GNU" },
    { ELFOSABI_SOLARIS, "UNIX - Solaris" },
    { ELFOSABI_AIX, "UNIX - AIX" },
    { ELFOSABI_IRIX, "UNIX - IRIX" },
    { ELFOSABI_FREEBSD, "UNIX - FreeBSD" },
    { ELFOSABI_TRU64, "UNIX - TRU64" },
    { ELFOSABI_MODESTO, "Novell - Modesto" },
    { ELFOSABI_OPENBSD, "UNIX - OpenBSD" },
    { ELFOSABI_STANDALONE, "Standalone App" },
};

static const struct name_entry section_types[] = {
    { SHT_NULL, "NULL" },
    { SHT_PROGBITS, "PROGBITS" },
    { SHT_SYMTAB, "SYMTAB" },
    { SHT_STRTAB, "STRTAB" },
    { SHT_RELA, "RELA" },
    { SHT_HASH, "HASH" },
    { SHT_DYNAMIC, "DYNAMIC" },
    { SHT_NOTE, "NOTE" },
    { SHT_NOBITS, "NOBITS" },
    { SHT_REL, "REL" },
    { SHT_SHLIB, "SHLIB" },
    { SHT_DYNSYM, "DYNSYM" },
    { SHT_INIT_ARRAY, "INIT_ARRAY" },
    { SHT_FINI_ARRAY, "FINI_ARRAY" },
    { SHT_PREINIT_ARRAY, "PREINIT_ARRAY" },
    { SHT_GROUP, "GROUP" },
    { SHT_SYMTAB_SHNDX, "SYMTAB SECTION INDICES" },
    { SHT_RELR, "RELR" },
    { SHT_GNU_ATTRIBUTES, "GNU_ATTRIBUTES" },
    { SHT_GNU_HASH, "GNU_HASH" },
    { SHT_GNU_LIBLIST, "GNU_LIBLIST" },
    { SHT_CHECKSUM, "CHECKSUM" },
    { SHT_GNU_verdef, "VERDEF" },
    { SHT_GNU_verneed, "VERNEED" },
    { SHT_GNU_versym, "VERSYM" },
};

static const struct name_entry segment_types[] = {
    { PT_NULL, "NULL" },
    { PT_LOAD, "LOAD" },
    { PT_DYNAMIC, "DYNAMIC" },
    { PT_INTERP, "INTERP" },
    { PT_NOTE, "NOTE" },
    { PT_SHLIB, "SHLIB" },
    { PT_PHDR, "PHDR" },
    { PT_TLS, "TLS" },
    { PT_GNU_EH_FRAME, "GNU_EH_FRAME" },
    { PT_GNU_STACK, "GNU_STACK" },
    { PT_GNU_RELRO, "GNU_RELRO" },
    { PT_GNU_PROPERTY, "GNU_PROPERTY" },
    { PT_GNU_SFRAME, "GNU_SFRAME" },
};

static const struct name_entry symbol_types[] = {
    { STT_NOTYPE, "NOTYPE" },
    { STT_OBJECT, "OBJECT" },
    { STT_FUNC, "FUNC" },
    { STT_SECTION, "SECTION" },
    { STT_FILE, "FILE" },
    { STT_COMMON, "COMMON" },
    { STT_TLS, "TLS" },
    { STT_GNU_IFUNC, "IFUNC" },
};

static const struct name_entry symbol_binds[] = {
    { STB_LOCAL, "LOCAL" },
    { STB_GLOBAL, "GLOBAL" },
    { STB_WEAK, "WEAK" },
    { STB_GNU_UNIQUE, "UNIQUE" },
};

static const struct name_entry visibilities[] = {
    { STV_DEFAULT, "DEFAULT" },
    { STV_INTERNAL, "INTERNAL" },
    { STV_HIDDEN, "HIDDEN" },
    { STV_PROTECTED, "PROTECTED" },
};

static const struct name_entry dynamic_tags[] = {
    { DT_NULL, "NULL" },
    { DT_NEEDED, "NEEDED" },
    { DT_PLTRELSZ, "PLTRELSZ" },
    { DT_PLTGOT, "PLTGOT" },
    { DT_HASH, "HASH" },
    { DT_STRTAB, "STRTAB" },
    { DT_SYMTAB, "SYMTAB" },
    { DT_RELA, "RELA" },
    { DT_RELASZ, "RELASZ" },
    { DT_RELAENT, "RELAENT" },
    { DT_STRSZ, "STRSZ" },
    { DT_SYMENT, "SYMENT" },
    { DT_INIT, "INIT" },
    { DT_FINI, "FINI" },
    { DT_SONAME, "SONAME" },
    { DT_RPATH, "RPATH" },
    { DT_SYMBOLIC, "SYMBOLIC" },
    { DT_REL, "REL" },
    { DT_RELSZ, "RELSZ" },
    { DT_RELENT, "RELENT" },
    { DT_PLTREL, "PLTREL" },
    { DT_DEBUG, "DEBUG" },
    { DT_TEXTREL, "TEXTREL" },
    { DT_JMPREL, "JMPREL" },
    { DT_BIND_NOW, "BIND_NOW" },
    { DT_INIT_ARRAY, "INIT_ARRAY" },
    { DT_FINI_ARRAY, "FINI_ARRAY" },
    { DT_INIT_ARRAYSZ, "INIT_ARRAYSZ" },
    { DT_FINI_ARRAYSZ, "FINI_ARRAYSZ" },
    { DT_RUNPATH, "RUNPATH" },
    { DT_FLAGS, "FLAGS" },
    { DT_PREINIT_ARRAY, "PREINIT_ARRAY" },
    { DT_PREINIT_ARRAYSZ, "PREINIT_ARRAYSZ" },
    { DT_SYMTAB_SHNDX, "SYMTAB_SHNDX" },
    { DT_RELRSZ, "RELRSZ" },
    { DT_RELR, "RELR" },
    { DT_RELRENT, "RELRENT" },
    { DT_GNU_PRELINKED, "GNU_PRELINKED" },
    { DT_GNU_CONFLICTSZ, "GNU_CONFLICTSZ" },
    { DT_GNU_LIBLISTSZ, "GNU_LIBLISTSZ" },
    { DT_CHECKSUM, "CHECKSUM" },
    { DT_PLTPADSZ, "PLTPADSZ" },
    { DT_MOVEENT, "MOVEENT" },
    { DT_MOVESZ, "MOVESZ" },
    { DT_FEATURE_1, "FEATURE_1" },
    { DT_POSFLAG_1, "POSFLAG_1" },
    { DT_SYMINSZ, "SYMINSZ" },
    { DT_SYMINENT, "SYMINENT" },
    { DT_GNU_HASH, "GNU_HASH" },
    { DT_TLSDESC_PLT, "TLSDESC_PLT" },
    { DT_TLSDESC_GOT, "TLSDESC_GOT" },
    { DT_GNU_CONFLICT, "GNU_CONFLICT" },
    { DT_GNU_LIBLIST, "GNU_LIBLIST" },
    { DT_CONFIG, "CONFIG" },
    { DT_DEPAUDIT, "DEPAUDIT" },
    { DT_AUDIT, "AUDIT" },
    { DT_PLTPAD, "PLTPAD" },
    { DT_MOVETAB, "MOVETAB" },
    { DT_SYMINFO, "SYMINFO" },
    { DT_VERSYM, "VERSYM" },
    { DT_RELACOUNT, "RELACOUNT" },
    { DT_RELCOUNT, "RELCOUNT" },
    { DT_FLAGS_1, "FLAGS_1" },
    { DT_VERDEF, "VERDEF" },
    { DT_VERDEFNUM, "VERDEFNUM" },
    { DT_VERNEED, "VERNEED" },
    { DT_VERNEEDNUM, "VERNEEDNUM" },
};

/* The relocation tables are generated from the R_X86_64_ and R_AARCH64_
 * constants of <elf.h>.  An alias of a value is left out. */
static const struct name_entry x86_64_relocs[] = {
    { 0, "R_X86_64_NONE" },
    { 1, "R_X86_64_64" },
    { 2, "R_X86_64_PC32" },
    { 3, "R_X86_64_GOT32" },
    { 4, "R_X86_64_PLT32" },
    { 5, "R_X86_64_COPY" },
    { 6, "R_X86_64_GLOB_DAT" },
    { 7, "R_X86_64_JUMP_SLOT" },
    { 8, "R_X86_64_RELATIVE" },
    { 9, "R_X86_64_GOTPCREL" },
    { 10, "R_X86_64_32" },
    { 11, "R_X86_64_32S" },
    { 12, "R_X86_64_16" },
    { 13, "R_X86_64_PC16" },
    { 14, "R_X86_64_8" },
    { 15, "R_X86_64_PC8" },
    { 16, "R_X86_64_DTPMOD64" },
    { 17, "R_X86_64_DTPOFF64" },
    { 18, "R_X86_64_TPOFF64" },
    { 19, "R_X86_64_TLSGD" },
    { 20, "R_X86_64_TLSLD" },
    { 21, "R_X86_64_DTPOFF32" },
    { 22, "R_X86_64_GOTTPOFF" },
    { 23, "R_X86_64_TPOFF32" },
    { 24, "R_X86_64_PC64" },
    { 25, "R_X86_64_GOTOFF64" },
    { 26, "R_X86_64_GOTPC32" },
    { 27, "R_X86_64_GOT64" },
    { 28, "R_X86_64_GOTPCREL64" },
    { 29, "R_X86_64_GOTPC64" },
    { 30, "R_X86_64_GOTPLT64" },
    { 31, "R_X86_64_PLTOFF64" },
    { 32, "R_X86_64_SIZE32" },
    { 33, "R_X86_64_SIZE64" },
    { 34, "R_X86_64_GOTPC32_TLSDESC" },
    { 35, "R_X86_64_TLSDESC_CALL" },
    { 36, "R_X86_64_TLSDESC" },
    { 37, "R_X86_64_IRELATIVE" },
    { 38, "R_X86_64_RELATIVE64" },
    { 41, "R_X86_64_GOTPCRELX" },
    { 42, "R_X86_64_REX_GOTPCRELX" },
};

static const struct name_entry aarch64_relocs[] = {
    { 0, "R_AARCH64_NONE" },
    { 257, "R_AARCH64_ABS64" },
    { 258, "R_AARCH64_ABS32" },
    { 259, "R_AARCH64_ABS16" },
    { 260, "R_AARCH64_PREL64" },
    { 261, "R_AARCH64_PREL32" },
    { 262, "R_AARCH64_PREL16" },
    { 263, "R_AARCH64_MOVW_UABS_G0" },
    { 264, "R_AARCH64_MOVW_UABS_G0_NC" },
    { 265, "R_AARCH64_MOVW_UABS_G1" },
    { 266, "R_AARCH64_MOVW_UABS_G1_NC" },
    { 267, "R_AARCH64_MOVW_UABS_G2" },
    { 268, "R_AARCH64_MOVW_UABS_G2_NC" },
    { 269, "R_AARCH64_MOVW_UABS_G3" },
    { 270, "R_AARCH64_MOVW_SABS_G0" },
    { 271, "R_AARCH64_MOVW_SABS_G1" },
    { 272, "R_AARCH64_MOVW_SABS_G2" },
    { 273, "R_AARCH64_LD_PREL_LO19" },
    { 274, "R_AARCH64_ADR_PREL_LO21" },
    { 275, "R_AARCH64_ADR_PREL_PG_HI21" },
    { 276, "R_AARCH64_ADR_PREL_PG_HI21_NC" },
    { 277, "R_AARCH64_ADD_ABS_LO12_NC" },
    { 278, "R_AARCH64_LDST8_ABS_LO12_NC" },
    { 279, "R_AARCH64_TSTBR14" },
    { 280, "R_AARCH64_CONDBR19" },
    { 282, "R_AARCH64_JUMP26" },
    { 283, "R_AARCH64_CALL26" },
    { 284, "R_AARCH64_LDST16_ABS_LO12_NC" },
    { 285, "R_AARCH64_LDST32_ABS_LO12_NC" },
    { 286, "R_AARCH64_LDST64_ABS_LO12_NC" },
    { 287, "R_AARCH64_MOVW_PREL_G0" },
    { 288, "R_AARCH64_MOVW_PREL_G0_NC" },
    { 289, "R_AARCH64_MOVW_PREL_G1" },
    { 290, "R_AARCH64_MOVW_PREL_G1_NC" },
    { 291, "R_AARCH64_MOVW_PREL_G2" },
    { 292, "R_AARCH64_MOVW_PREL_G2_NC" },
    { 293, "R_AARCH64_MOVW_PREL_G3" },
    { 299, "R_AARCH64_LDST128_ABS_LO12_NC" },
    { 300, "R_AARCH64_MOVW_GOTOFF_G0" },
    { 301, "R_AARCH64_MOVW_GOTOFF_G0_NC" },
    { 302, "R_AARCH64_MOVW_GOTOFF_G1" },
    { 303, "R_AARCH64_MOVW_GOTOFF_G1_NC" },
    { 304, "R_AARCH64_MOVW_GOTOFF_G2" },
    { 305, "R_AARCH64_MOVW_GOTOFF_G2_NC" },
    { 306, "R_AARCH64_MOVW_GOTOFF_G3" },
    { 307, "R_AARCH64_GOTREL64" },
    { 308, "R_AARCH64_GOTREL32" },
    { 309, "R_AARCH64_GOT_LD_PREL19" },
    { 310, "R_AARCH64_LD64_GOTOFF_LO15" },
    { 311, "R_AARCH64_ADR_GOT_PAGE" },
    { 312, "R_AARCH64_LD64_GOT_LO12_NC" },
    { 313, "R_AARCH64_LD64_GOTPAGE_LO15" },
    { 512, "R_AARCH64_TLSGD_ADR_PREL21" },
    { 513, "R_AARCH64_TLSGD_ADR_PAGE21" },
    { 514, "R_AARCH64_TLSGD_ADD_LO12_NC" },
    { 515, "R_AARCH64_TLSGD_MOVW_G1" },
    { 516, "R_AARCH64_TLSGD_MOVW_G0_NC" },
    { 517, "R_AARCH64_TLSLD_ADR_PREL21" },
    { 518, "R_AARCH64_TLSLD_ADR_PAGE21" },
    { 519, "R_AARCH64_TLSLD_ADD_LO12_NC" },
    { 539, "R_AARCH64_TLSIE_MOVW_GOTTPREL_G1" },
    { 540, "R_AARCH64_TLSIE_MOVW_GOTTPREL_G0_NC" },
    { 541, "R_AARCH64_TLSIE_ADR_GOTTPREL_PAGE21" },
    { 542, "R_AARCH64_TLSIE_LD64_GOTTPREL_LO12_NC" },
    { 543, "R_AARCH64_TLSIE_LD_GOTTPREL_PREL19" },
    { 544, "R_AARCH64_TLSLE_MOVW_TPREL_G2" },
    { 545, "R_AARCH64_TLSLE_MOVW_TPREL_G1" },
    { 546, "R_AARCH64_TLSLE_MOVW_TPREL_G1_NC" },
    { 547, "R_AARCH64_TLSLE_MOVW_TPREL_G0" },
    { 548, "R_AARCH64_TLSLE_MOVW_TPREL_G0_NC" },
    { 549, "R_AARCH64_TLSLE_ADD_TPREL_HI12" },
    { 550, "R_AARCH64_TLSLE_ADD_TPREL_LO12" },
    { 551, "R_AARCH64_TLSLE_ADD_TPREL_LO12_NC" },
    { 552, "R_AARCH64_TLSLE_LDST8_TPREL_LO12" },
    { 553, "R_AARCH64_TLSLE_LDST8_TPREL_LO12_NC" },
    { 554, "R_AARCH64_TLSLE_LDST16_TPREL_LO12" },
    { 555, "R_AARCH64_TLSLE_LDST16_TPREL_LO12_NC" },
    { 556, "R_AARCH64_TLSLE_LDST32_TPREL_LO12" },
    { 557, "R_AARCH64_TLSLE_LDST32_TPREL_LO12_NC" },
    { 558, "R_AARCH64_TLSLE_LDST64_TPREL_LO12" },
    { 559, "R_AARCH64_TLSLE_LDST64_TPREL_LO12_NC" },
    { 560, "R_AARCH64_TLSDESC_LD_PREL19" },
    { 561, "R_AARCH64_TLSDESC_ADR_PREL21" },
    { 562, "R_AARCH64_TLSDESC_ADR_PAGE21" },
    { 563, "R_AARCH64_TLSDESC_LD64_LO12" },
    { 564, "R_AARCH64_TLSDESC_ADD_LO12" },
    { 565, "R_AARCH64_TLSDESC_OFF_G1" },
    { 566, "R_AARCH64_TLSDESC_OFF_G0_NC" },
    { 567, "R_AARCH64_TLSDESC_LDR" },
    { 568, "R_AARCH64_TLSDESC_ADD" },
    { 569, "R_AARCH64_TLSDESC_CALL" },
    { 570, "R_AARCH64_TLSLE_LDST128_TPREL_LO12" },
    { 571, "R_AARCH64_TLSLE_LDST128_TPREL_LO12_NC" },
    { 572, "R_AARCH64_TLSLD_LDST128_DTPREL_LO12" },
    { 573, "R_AARCH64_TLSLD_LDST128_DTPREL_LO12_NC" },
    { 1024, "R_AARCH64_COPY" },
    { 1025, "R_AARCH64_GLOB_DAT" },
    { 1026, "R_AARCH64_JUMP_SLOT" },
    { 1027, "R_AARCH64_RELATIVE" },
    { 1028, "R_AARCH64_TLS_DTPMOD" },
    { 1029, "R_AARCH64_TLS_DTPREL" },
    { 1030, "R_AARCH64_TLS_TPREL" },
    { 1031, "R_AARCH64_TLSDESC" },
    { 1032, "R_AARCH64_IRELATIVE" },
};

const char *elffile_type_name(unsigned type) { return LOOKUP(types, type); }
const char *elffile_machine_name(unsigned machine) { return LOOKUP(machines, machine); }
const char *elffile_osabi_name(unsigned osabi) { return LOOKUP(osabis, osabi); }
const char *elffile_symbol_type_name(unsigned type) { return LOOKUP(symbol_types, type); }
const char *elffile_symbol_bind_name(unsigned bind) { return LOOKUP(symbol_binds, bind); }
const char *elffile_symbol_visibility_name(unsigned vis) { return LOOKUP(visibilities, vis); }

const char *elffile_section_type_name(unsigned machine, uint32_t type)
{
    if (machine == EM_X86_64 && type == SHT_X86_64_UNWIND)
        return "X86_64_UNWIND";
    if (machine == EM_AARCH64 && type == SHT_AARCH64_ATTRIBUTES)
        return "AARCH64_ATTRIBUTES";
    return LOOKUP(section_types, type);
}

const char *elffile_segment_type_name(unsigned machine, uint32_t type)
{
    if (machine == EM_AARCH64 && type == PT_AARCH64_MEMTAG_MTE)
        return "AARCH64_MEMTAG_MTE";
    return LOOKUP(segment_types, type);
}

const char *elffile_dynamic_tag_name(unsigned machine, int64_t tag)
{
    if (machine == EM_AARCH64) {
        if (tag == DT_AARCH64_BTI_PLT)
            return "AARCH64_BTI_PLT";
        if (tag == DT_AARCH64_PAC_PLT)
            return "AARCH64_PAC_PLT";
        if (tag == DT_AARCH64_VARIANT_PCS)
            return "AARCH64_VARIANT_PCS";
    }
    return LOOKUP(dynamic_tags, tag);
}

const char *elffile_reloc_name(unsigned machine, uint32_t type)
{
    if (machine == EM_X86_64)
        return LOOKUP(x86_64_relocs, type);
    if (machine == EM_AARCH64)
        return LOOKUP(aarch64_relocs, type);
    return NULL;
}

const char *elffile_bfd_name(unsigned machine)
{
    switch (machine) {
    case EM_X86_64:  return "elf64-x86-64";
    case EM_AARCH64: return "elf64-littleaarch64";
    default:         return NULL;
    }
}
