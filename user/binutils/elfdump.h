#pragma once
/* The code that readelf and objdump share: the relocation entries, the
 * dynamic section with its string table, the selection of sections by
 * number or name and the symbol versions (docs/design/binutils.md).  The
 * build links elfdump.c into both programs. */
#include "lib/binutils.h"
#include <stddef.h>
#include <stdint.h>

/* ---- sections ---- */

/* elfdump_section_matches reports whether the section at index matches
 * spec, a decimal section number or a section name. */
int elfdump_section_matches(const struct elffile *f, unsigned index, const char *spec);
/* elfdump_vma_to_offset maps the address vma of a range of size bytes to
 * the file offset through the LOAD segments and stores it in *off.  The
 * result is 0 or -ERANGE. */
int elfdump_vma_to_offset(const struct elffile *f, uint64_t vma, uint64_t size, uint64_t *off);

/* ---- relocations ---- */

struct elfdump_reloc {
    uint64_t offset;
    uint64_t info;
    int64_t addend;             /* 0 for a section of the type SHT_REL */
};

/* elfdump_read_relocs decodes the entries of the relocation section sh
 * of the type SHT_REL or SHT_RELA.  *out receives memory that the caller
 * frees.  The result is the number of entries or a negative errno. */
long elfdump_read_relocs(const struct elffile *f, const Elf64_Shdr *sh, struct elfdump_reloc **out);

/* ---- dynamic section ---- */

struct elfdump_dynamic {
    const Elf64_Dyn *entries;
    size_t count;               /* the entries up to and including DT_NULL */
    uint64_t offset;            /* the file offset of the section */
    const char *strtab;         /* the dynamic string table or NULL */
    uint64_t strsz;
};

/* elfdump_load_dynamic finds the dynamic section through the PT_DYNAMIC
 * segment or else through the section header.  The result is 0, or
 * -ENOENT without a dynamic section. */
int elfdump_load_dynamic(const struct elffile *f, struct elfdump_dynamic *d);
/* elfdump_dynamic_value returns the value of the first entry with tag,
 * and stores 1 in *found when the entry exists. */
uint64_t elfdump_dynamic_value(const struct elfdump_dynamic *d, int64_t tag, int *found);
/* elfdump_dynamic_string returns the string at offset off of the dynamic
 * string table, or NULL. */
const char *elfdump_dynamic_string(const struct elfdump_dynamic *d, uint64_t off);

/* ---- symbol versions ---- */

/* The version information of a dynamic symbol table.  The tables point
 * into the file. */
struct elfdump_versions {
    const Elf64_Shdr *versym_sh;
    const Elf64_Shdr *verdef_sh;
    const Elf64_Shdr *verneed_sh;
    const uint16_t *versym;     /* one entry per symbol of the table */
    size_t nversym;
};

/* elfdump_versions_init collects the version sections of the file.  The
 * version symbol section is the one that links to the symbol table
 * section symtab. */
void elfdump_versions_init(const struct elffile *f, const Elf64_Shdr *symtab, struct elfdump_versions *v);
/* elfdump_version_name returns the name of the version with the number
 * index, from the definitions or else from the requirements.  *is_need
 * receives 1 for a requirement.  The result is NULL for an unknown
 * number. */
const char *elfdump_version_name(const struct elffile *f, const struct elfdump_versions *v, unsigned index,
                                 int *is_need);
/* elfdump_format_symbol_name returns the name of the symbol number n of a
 * dynamic symbol table with the version suffix.  A version requirement
 * carries its number in parentheses when need_index is set.  The result is
 * valid until the next call. */
const char *elfdump_format_symbol_name(const struct elffile *f, const struct elfdump_versions *v,
                                       const Elf64_Shdr *strtab, const Elf64_Sym *sym, size_t n, int need_index);
