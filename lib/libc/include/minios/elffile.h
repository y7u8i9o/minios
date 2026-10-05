#pragma once
/* A reader of ELF64 little-endian files in memory, for pkg, the binutils
 * and libprof.  elffile_open checks the file header, the section header
 * table and the program header table against the size of the file.  The
 * other functions check every offset and size that they follow, and they
 * return NULL or 0 for a part that lies outside the file.  The reader
 * does not copy the file.  The pointers that it returns point into the
 * data that the caller passed to elffile_open. */
#include <elf.h>
#include <stddef.h>
#include <stdint.h>

struct elffile {
    const uint8_t *data;
    size_t size;
    const Elf64_Ehdr *eh;
    const Elf64_Shdr *sh;       /* the section headers, NULL without them */
    unsigned nsections;
    const Elf64_Phdr *ph;       /* the program headers, NULL without them */
    unsigned nsegments;
    const Elf64_Shdr *shstrtab; /* the section name table, NULL without it */
};

/* elffile_open checks data as an ELF64 little-endian file of size bytes.
 * The result is 0, or -ENOEXEC when the data is no such file, or -EINVAL
 * when a header table lies outside the data. */
int elffile_open(struct elffile *f, const void *data, size_t size);
/* elffile_load reads the file at path into memory and opens it.  *buf
 * receives the memory, which the caller frees.  The result is 0 or a
 * negative errno. */
int elffile_load(struct elffile *f, const char *path, void **buf);

/* elffile_section returns the section header at index, or NULL. */
const Elf64_Shdr *elffile_section(const struct elffile *f, unsigned index);
/* elffile_section_index returns the index of the section header s. */
unsigned elffile_section_index(const struct elffile *f, const Elf64_Shdr *s);
/* elffile_section_name returns the name of s, or "" when the name lies
 * outside the section name table. */
const char *elffile_section_name(const struct elffile *f, const Elf64_Shdr *s);
/* elffile_find_section returns the first section with the name, or NULL. */
const Elf64_Shdr *elffile_find_section(const struct elffile *f, const char *name);
/* elffile_find_type returns the first section of the type, or NULL. */
const Elf64_Shdr *elffile_find_type(const struct elffile *f, uint32_t type);
/* elffile_section_data returns the contents of s, or NULL for a section
 * of the type SHT_NOBITS or a section outside the file. */
const void *elffile_section_data(const struct elffile *f, const Elf64_Shdr *s);
/* elffile_linked returns the section that the sh_link field of s gives,
 * or NULL. */
const Elf64_Shdr *elffile_linked(const struct elffile *f, const Elf64_Shdr *s);

/* elffile_string returns the string at offset off of the string table
 * strtab, or NULL when the string is outside the table or has no end. */
const char *elffile_string(const struct elffile *f, const Elf64_Shdr *strtab, uint64_t off);

/* elffile_symbols returns the symbols of the symbol table section
 * symtab, of the type SHT_SYMTAB or SHT_DYNSYM, and stores their number
 * in *count and their string table in *strtab.  The result is NULL when
 * the table or its string table is invalid. */
const Elf64_Sym *elffile_symbols(const struct elffile *f, const Elf64_Shdr *symtab, size_t *count,
                                 const Elf64_Shdr **strtab);
/* elffile_symbol_name returns the name of a symbol of a table that
 * elffile_symbols returned.  A section symbol without a name receives the
 * name of its section.  The result is "" for an invalid name. */
const char *elffile_symbol_name(const struct elffile *f, const Elf64_Shdr *strtab, const Elf64_Sym *sym);

/* elffile_dynamic returns the entries of the dynamic section and stores
 * their number, up to and excluding DT_NULL, in *count, and the string
 * table of the section in *strtab.  The result is NULL without a valid
 * dynamic section. */
const Elf64_Dyn *elffile_dynamic(const struct elffile *f, size_t *count, const Elf64_Shdr **strtab);

/* The names of the constants, as the GNU binutils print them.  An unknown
 * value gives NULL. */
const char *elffile_type_name(unsigned type);              /* "REL (Relocatable file)" */
const char *elffile_machine_name(unsigned machine);        /* "Advanced Micro Devices X86-64" */
const char *elffile_osabi_name(unsigned osabi);            /* "UNIX - System V" */
const char *elffile_section_type_name(unsigned machine, uint32_t type);   /* "PROGBITS" */
const char *elffile_segment_type_name(unsigned machine, uint32_t type);   /* "LOAD" */
const char *elffile_symbol_type_name(unsigned type);       /* "FUNC" */
const char *elffile_symbol_bind_name(unsigned bind);       /* "GLOBAL" */
const char *elffile_symbol_visibility_name(unsigned vis);  /* "DEFAULT" */
const char *elffile_dynamic_tag_name(unsigned machine, int64_t tag);       /* "NEEDED" */
const char *elffile_reloc_name(unsigned machine, uint32_t type);          /* "R_X86_64_PC32" */
/* elffile_bfd_name returns the name of the target of a machine in the
 * manner of objdump: "elf64-x86-64" and "elf64-littleaarch64". */
const char *elffile_bfd_name(unsigned machine);
