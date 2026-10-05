#pragma once
/* The ELF format: the types and constants of the System V ABI and of the
 * processor supplements for x86_64 and aarch64, with the names that other
 * systems use in <elf.h>.  minios reads and writes ELF64 files; the ELF32
 * types are defined for programs that name them. */
#include <stdint.h>

typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Word;
typedef int32_t  Elf32_Sword;
typedef uint32_t Elf32_Addr;
typedef uint32_t Elf32_Off;
typedef uint16_t Elf32_Section;

typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef int32_t  Elf64_Sword;
typedef uint64_t Elf64_Xword;
typedef int64_t  Elf64_Sxword;
typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Section;
typedef Elf64_Half Elf64_Versym;

/* ---- file header ---- */

#define EI_NIDENT 16

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    Elf32_Half e_type;
    Elf32_Half e_machine;
    Elf32_Word e_version;
    Elf32_Addr e_entry;
    Elf32_Off e_phoff;
    Elf32_Off e_shoff;
    Elf32_Word e_flags;
    Elf32_Half e_ehsize;
    Elf32_Half e_phentsize;
    Elf32_Half e_phnum;
    Elf32_Half e_shentsize;
    Elf32_Half e_shnum;
    Elf32_Half e_shstrndx;
} Elf32_Ehdr;

typedef struct {
    unsigned char e_ident[EI_NIDENT];
    Elf64_Half e_type;
    Elf64_Half e_machine;
    Elf64_Word e_version;
    Elf64_Addr e_entry;
    Elf64_Off e_phoff;
    Elf64_Off e_shoff;
    Elf64_Word e_flags;
    Elf64_Half e_ehsize;
    Elf64_Half e_phentsize;
    Elf64_Half e_phnum;
    Elf64_Half e_shentsize;
    Elf64_Half e_shnum;
    Elf64_Half e_shstrndx;
} Elf64_Ehdr;

#define EI_MAG0 0
#define EI_MAG1 1
#define EI_MAG2 2
#define EI_MAG3 3
#define EI_CLASS 4
#define EI_DATA 5
#define EI_VERSION 6
#define EI_OSABI 7
#define EI_ABIVERSION 8
#define EI_PAD 9

#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'
#define ELFMAG "\177ELF"
#define SELFMAG 4

#define ELFCLASSNONE 0
#define ELFCLASS32 1
#define ELFCLASS64 2

#define ELFDATANONE 0
#define ELFDATA2LSB 1
#define ELFDATA2MSB 2

#define EV_NONE 0
#define EV_CURRENT 1

#define ELFOSABI_NONE 0
#define ELFOSABI_SYSV 0
#define ELFOSABI_HPUX 1
#define ELFOSABI_NETBSD 2
#define ELFOSABI_GNU 3
#define ELFOSABI_LINUX ELFOSABI_GNU
#define ELFOSABI_SOLARIS 6
#define ELFOSABI_AIX 7
#define ELFOSABI_IRIX 8
#define ELFOSABI_FREEBSD 9
#define ELFOSABI_TRU64 10
#define ELFOSABI_MODESTO 11
#define ELFOSABI_OPENBSD 12
#define ELFOSABI_ARM 97
#define ELFOSABI_STANDALONE 255

#define ET_NONE 0
#define ET_REL 1
#define ET_EXEC 2
#define ET_DYN 3
#define ET_CORE 4
#define ET_LOOS 0xfe00
#define ET_HIOS 0xfeff
#define ET_LOPROC 0xff00
#define ET_HIPROC 0xffff

#define EM_NONE 0
#define EM_SPARC 2
#define EM_386 3
#define EM_68K 4
#define EM_MIPS 8
#define EM_PPC 20
#define EM_PPC64 21
#define EM_S390 22
#define EM_ARM 40
#define EM_SPARCV9 43
#define EM_IA_64 50
#define EM_X86_64 62
#define EM_AARCH64 183
#define EM_RISCV 243
#define EM_BPF 247
#define EM_LOONGARCH 258

/* ---- section headers ---- */

typedef struct {
    Elf32_Word sh_name;
    Elf32_Word sh_type;
    Elf32_Word sh_flags;
    Elf32_Addr sh_addr;
    Elf32_Off sh_offset;
    Elf32_Word sh_size;
    Elf32_Word sh_link;
    Elf32_Word sh_info;
    Elf32_Word sh_addralign;
    Elf32_Word sh_entsize;
} Elf32_Shdr;

typedef struct {
    Elf64_Word sh_name;
    Elf64_Word sh_type;
    Elf64_Xword sh_flags;
    Elf64_Addr sh_addr;
    Elf64_Off sh_offset;
    Elf64_Xword sh_size;
    Elf64_Word sh_link;
    Elf64_Word sh_info;
    Elf64_Xword sh_addralign;
    Elf64_Xword sh_entsize;
} Elf64_Shdr;

#define SHN_UNDEF 0
#define SHN_LORESERVE 0xff00
#define SHN_LOPROC 0xff00
#define SHN_HIPROC 0xff1f
#define SHN_LOOS 0xff20
#define SHN_HIOS 0xff3f
#define SHN_ABS 0xfff1
#define SHN_COMMON 0xfff2
#define SHN_XINDEX 0xffff
#define SHN_HIRESERVE 0xffff

#define SHT_NULL 0
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_HASH 5
#define SHT_DYNAMIC 6
#define SHT_NOTE 7
#define SHT_NOBITS 8
#define SHT_REL 9
#define SHT_SHLIB 10
#define SHT_DYNSYM 11
#define SHT_INIT_ARRAY 14
#define SHT_FINI_ARRAY 15
#define SHT_PREINIT_ARRAY 16
#define SHT_GROUP 17
#define SHT_SYMTAB_SHNDX 18
#define SHT_RELR 19
#define SHT_NUM 20
#define SHT_LOOS 0x60000000
#define SHT_GNU_ATTRIBUTES 0x6ffffff5
#define SHT_GNU_HASH 0x6ffffff6
#define SHT_GNU_LIBLIST 0x6ffffff7
#define SHT_CHECKSUM 0x6ffffff8
#define SHT_GNU_verdef 0x6ffffffd
#define SHT_GNU_verneed 0x6ffffffe
#define SHT_GNU_versym 0x6fffffff
#define SHT_HIOS 0x6fffffff
#define SHT_LOPROC 0x70000000
#define SHT_X86_64_UNWIND 0x70000001
#define SHT_AARCH64_ATTRIBUTES 0x70000003
#define SHT_HIPROC 0x7fffffff
#define SHT_LOUSER 0x80000000
#define SHT_HIUSER 0x8fffffff

#define SHF_WRITE 0x1
#define SHF_ALLOC 0x2
#define SHF_EXECINSTR 0x4
#define SHF_MERGE 0x10
#define SHF_STRINGS 0x20
#define SHF_INFO_LINK 0x40
#define SHF_LINK_ORDER 0x80
#define SHF_OS_NONCONFORMING 0x100
#define SHF_GROUP 0x200
#define SHF_TLS 0x400
#define SHF_COMPRESSED 0x800
#define SHF_MASKOS 0x0ff00000
#define SHF_GNU_RETAIN 0x200000
#define SHF_MASKPROC 0xf0000000
#define SHF_X86_64_LARGE 0x10000000
#define SHF_EXCLUDE 0x80000000

#define GRP_COMDAT 0x1

typedef struct {
    Elf64_Word ch_type;
    Elf64_Word ch_reserved;
    Elf64_Xword ch_size;
    Elf64_Xword ch_addralign;
} Elf64_Chdr;

#define ELFCOMPRESS_ZLIB 1
#define ELFCOMPRESS_ZSTD 2

/* ---- symbols ---- */

typedef struct {
    Elf32_Word st_name;
    Elf32_Addr st_value;
    Elf32_Word st_size;
    unsigned char st_info;
    unsigned char st_other;
    Elf32_Section st_shndx;
} Elf32_Sym;

typedef struct {
    Elf64_Word st_name;
    unsigned char st_info;
    unsigned char st_other;
    Elf64_Section st_shndx;
    Elf64_Addr st_value;
    Elf64_Xword st_size;
} Elf64_Sym;

#define ELF32_ST_BIND(i) ((i) >> 4)
#define ELF32_ST_TYPE(i) ((i) & 0xf)
#define ELF32_ST_INFO(b, t) (((b) << 4) + ((t) & 0xf))
#define ELF64_ST_BIND(i) ((i) >> 4)
#define ELF64_ST_TYPE(i) ((i) & 0xf)
#define ELF64_ST_INFO(b, t) (((b) << 4) + ((t) & 0xf))
#define ELF32_ST_VISIBILITY(o) ((o) & 0x3)
#define ELF64_ST_VISIBILITY(o) ((o) & 0x3)

#define STN_UNDEF 0

#define STB_LOCAL 0
#define STB_GLOBAL 1
#define STB_WEAK 2
#define STB_GNU_UNIQUE 10
#define STB_LOOS 10
#define STB_HIOS 12
#define STB_LOPROC 13
#define STB_HIPROC 15

#define STT_NOTYPE 0
#define STT_OBJECT 1
#define STT_FUNC 2
#define STT_SECTION 3
#define STT_FILE 4
#define STT_COMMON 5
#define STT_TLS 6
#define STT_GNU_IFUNC 10
#define STT_LOOS 10
#define STT_HIOS 12
#define STT_LOPROC 13
#define STT_HIPROC 15

#define STV_DEFAULT 0
#define STV_INTERNAL 1
#define STV_HIDDEN 2
#define STV_PROTECTED 3

/* ---- relocations ---- */

typedef struct {
    Elf32_Addr r_offset;
    Elf32_Word r_info;
} Elf32_Rel;

typedef struct {
    Elf32_Addr r_offset;
    Elf32_Word r_info;
    Elf32_Sword r_addend;
} Elf32_Rela;

typedef struct {
    Elf64_Addr r_offset;
    Elf64_Xword r_info;
} Elf64_Rel;

typedef struct {
    Elf64_Addr r_offset;
    Elf64_Xword r_info;
    Elf64_Sxword r_addend;
} Elf64_Rela;

typedef Elf64_Xword Elf64_Relr;

#define ELF32_R_SYM(i) ((i) >> 8)
#define ELF32_R_TYPE(i) ((unsigned char)(i))
#define ELF32_R_INFO(s, t) (((s) << 8) + (unsigned char)(t))
#define ELF64_R_SYM(i) ((i) >> 32)
#define ELF64_R_TYPE(i) ((i) & 0xffffffff)
#define ELF64_R_INFO(s, t) ((((Elf64_Xword)(s)) << 32) + (t))

/* ---- program headers ---- */

typedef struct {
    Elf32_Word p_type;
    Elf32_Off p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
} Elf32_Phdr;

typedef struct {
    Elf64_Word p_type;
    Elf64_Word p_flags;
    Elf64_Off p_offset;
    Elf64_Addr p_vaddr;
    Elf64_Addr p_paddr;
    Elf64_Xword p_filesz;
    Elf64_Xword p_memsz;
    Elf64_Xword p_align;
} Elf64_Phdr;

#define PN_XNUM 0xffff

#define PT_NULL 0
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define PT_INTERP 3
#define PT_NOTE 4
#define PT_SHLIB 5
#define PT_PHDR 6
#define PT_TLS 7
#define PT_NUM 8
#define PT_LOOS 0x60000000
#define PT_GNU_EH_FRAME 0x6474e550
#define PT_GNU_STACK 0x6474e551
#define PT_GNU_RELRO 0x6474e552
#define PT_GNU_PROPERTY 0x6474e553
#define PT_GNU_SFRAME 0x6474e554
#define PT_HIOS 0x6fffffff
#define PT_LOPROC 0x70000000
#define PT_AARCH64_MEMTAG_MTE 0x70000002
#define PT_HIPROC 0x7fffffff

#define PF_X 0x1
#define PF_W 0x2
#define PF_R 0x4
#define PF_MASKOS 0x0ff00000
#define PF_MASKPROC 0xf0000000

/* ---- dynamic section ---- */

typedef struct {
    Elf32_Sword d_tag;
    union {
        Elf32_Word d_val;
        Elf32_Addr d_ptr;
    } d_un;
} Elf32_Dyn;

typedef struct {
    Elf64_Sxword d_tag;
    union {
        Elf64_Xword d_val;
        Elf64_Addr d_ptr;
    } d_un;
} Elf64_Dyn;

#define DT_NULL 0
#define DT_NEEDED 1
#define DT_PLTRELSZ 2
#define DT_PLTGOT 3
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_RELA 7
#define DT_RELASZ 8
#define DT_RELAENT 9
#define DT_STRSZ 10
#define DT_SYMENT 11
#define DT_INIT 12
#define DT_FINI 13
#define DT_SONAME 14
#define DT_RPATH 15
#define DT_SYMBOLIC 16
#define DT_REL 17
#define DT_RELSZ 18
#define DT_RELENT 19
#define DT_PLTREL 20
#define DT_DEBUG 21
#define DT_TEXTREL 22
#define DT_JMPREL 23
#define DT_BIND_NOW 24
#define DT_INIT_ARRAY 25
#define DT_FINI_ARRAY 26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28
#define DT_RUNPATH 29
#define DT_FLAGS 30
#define DT_ENCODING 32
#define DT_PREINIT_ARRAY 32
#define DT_PREINIT_ARRAYSZ 33
#define DT_SYMTAB_SHNDX 34
#define DT_RELRSZ 35
#define DT_RELR 36
#define DT_RELRENT 37
#define DT_NUM 38
#define DT_LOOS 0x6000000d
#define DT_HIOS 0x6ffff000
#define DT_VALRNGLO 0x6ffffd00
#define DT_GNU_PRELINKED 0x6ffffdf5
#define DT_GNU_CONFLICTSZ 0x6ffffdf6
#define DT_GNU_LIBLISTSZ 0x6ffffdf7
#define DT_CHECKSUM 0x6ffffdf8
#define DT_PLTPADSZ 0x6ffffdf9
#define DT_MOVEENT 0x6ffffdfa
#define DT_MOVESZ 0x6ffffdfb
#define DT_FEATURE_1 0x6ffffdfc
#define DT_POSFLAG_1 0x6ffffdfd
#define DT_SYMINSZ 0x6ffffdfe
#define DT_SYMINENT 0x6ffffdff
#define DT_VALRNGHI 0x6ffffdff
#define DT_ADDRRNGLO 0x6ffffe00
#define DT_GNU_HASH 0x6ffffef5
#define DT_TLSDESC_PLT 0x6ffffef6
#define DT_TLSDESC_GOT 0x6ffffef7
#define DT_GNU_CONFLICT 0x6ffffef8
#define DT_GNU_LIBLIST 0x6ffffef9
#define DT_CONFIG 0x6ffffefa
#define DT_DEPAUDIT 0x6ffffefb
#define DT_AUDIT 0x6ffffefc
#define DT_PLTPAD 0x6ffffefd
#define DT_MOVETAB 0x6ffffefe
#define DT_SYMINFO 0x6ffffeff
#define DT_ADDRRNGHI 0x6ffffeff
#define DT_VERSYM 0x6ffffff0
#define DT_RELACOUNT 0x6ffffff9
#define DT_RELCOUNT 0x6ffffffa
#define DT_FLAGS_1 0x6ffffffb
#define DT_VERDEF 0x6ffffffc
#define DT_VERDEFNUM 0x6ffffffd
#define DT_VERNEED 0x6ffffffe
#define DT_VERNEEDNUM 0x6fffffff
#define DT_LOPROC 0x70000000
#define DT_AARCH64_BTI_PLT 0x70000001
#define DT_AARCH64_PAC_PLT 0x70000003
#define DT_AARCH64_VARIANT_PCS 0x70000005
#define DT_HIPROC 0x7fffffff

#define DF_ORIGIN 0x1
#define DF_SYMBOLIC 0x2
#define DF_TEXTREL 0x4
#define DF_BIND_NOW 0x8
#define DF_STATIC_TLS 0x10

#define DF_1_NOW 0x1
#define DF_1_GLOBAL 0x2
#define DF_1_GROUP 0x4
#define DF_1_NODELETE 0x8
#define DF_1_LOADFLTR 0x10
#define DF_1_INITFIRST 0x20
#define DF_1_NOOPEN 0x40
#define DF_1_ORIGIN 0x80
#define DF_1_DIRECT 0x100
#define DF_1_TRANS 0x200
#define DF_1_INTERPOSE 0x400
#define DF_1_NODEFLIB 0x800
#define DF_1_NODUMP 0x1000
#define DF_1_CONFALT 0x2000
#define DF_1_ENDFILTEE 0x4000
#define DF_1_DISPRELDNE 0x8000
#define DF_1_DISPRELPND 0x10000
#define DF_1_NODIRECT 0x20000
#define DF_1_IGNMULDEF 0x40000
#define DF_1_NOKSYMS 0x80000
#define DF_1_NOHDR 0x100000
#define DF_1_EDITED 0x200000
#define DF_1_NORELOC 0x400000
#define DF_1_SYMINTPOSE 0x800000
#define DF_1_GLOBAUDIT 0x1000000
#define DF_1_SINGLETON 0x2000000
#define DF_1_STUB 0x4000000
#define DF_1_PIE 0x8000000

/* ---- notes ---- */

typedef struct {
    Elf32_Word n_namesz;
    Elf32_Word n_descsz;
    Elf32_Word n_type;
} Elf32_Nhdr;

typedef struct {
    Elf64_Word n_namesz;
    Elf64_Word n_descsz;
    Elf64_Word n_type;
} Elf64_Nhdr;

#define NT_PRSTATUS 1
#define NT_PRFPREG 2
#define NT_PRPSINFO 3
#define NT_AUXV 6

#define NT_GNU_ABI_TAG 1
#define NT_GNU_HWCAP 2
#define NT_GNU_BUILD_ID 3
#define NT_GNU_GOLD_VERSION 4
#define NT_GNU_PROPERTY_TYPE_0 5

/* ---- symbol versions ---- */

typedef struct {
    Elf64_Half vd_version;
    Elf64_Half vd_flags;
    Elf64_Half vd_ndx;
    Elf64_Half vd_cnt;
    Elf64_Word vd_hash;
    Elf64_Word vd_aux;
    Elf64_Word vd_next;
} Elf64_Verdef;

typedef struct {
    Elf64_Word vda_name;
    Elf64_Word vda_next;
} Elf64_Verdaux;

typedef struct {
    Elf64_Half vn_version;
    Elf64_Half vn_cnt;
    Elf64_Word vn_file;
    Elf64_Word vn_aux;
    Elf64_Word vn_next;
} Elf64_Verneed;

typedef struct {
    Elf64_Word vna_hash;
    Elf64_Half vna_flags;
    Elf64_Half vna_other;
    Elf64_Word vna_name;
    Elf64_Word vna_next;
} Elf64_Vernaux;

#define VER_NDX_LOCAL 0
#define VER_NDX_GLOBAL 1
#define VER_FLG_BASE 0x1
#define VER_FLG_WEAK 0x2

/* ---- auxiliary vector ---- */

typedef struct {
    uint64_t a_type;
    union {
        uint64_t a_val;
    } a_un;
} Elf64_auxv_t;

#define AT_NULL 0
#define AT_IGNORE 1
#define AT_EXECFD 2
#define AT_PHDR 3
#define AT_PHENT 4
#define AT_PHNUM 5
#define AT_PAGESZ 6
#define AT_BASE 7
#define AT_FLAGS 8
#define AT_ENTRY 9
#define AT_NOTELF 10
#define AT_UID 11
#define AT_EUID 12
#define AT_GID 13
#define AT_EGID 14
#define AT_CLKTCK 17
#define AT_PLATFORM 15
#define AT_HWCAP 16
#define AT_SECURE 23
#define AT_RANDOM 25
#define AT_HWCAP2 26
#define AT_EXECFN 31

/* ---- x86_64 relocations ---- */

#define R_X86_64_NONE 0
#define R_X86_64_64 1
#define R_X86_64_PC32 2
#define R_X86_64_GOT32 3
#define R_X86_64_PLT32 4
#define R_X86_64_COPY 5
#define R_X86_64_GLOB_DAT 6
#define R_X86_64_JUMP_SLOT 7
#define R_X86_64_RELATIVE 8
#define R_X86_64_GOTPCREL 9
#define R_X86_64_32 10
#define R_X86_64_32S 11
#define R_X86_64_16 12
#define R_X86_64_PC16 13
#define R_X86_64_8 14
#define R_X86_64_PC8 15
#define R_X86_64_DTPMOD64 16
#define R_X86_64_DTPOFF64 17
#define R_X86_64_TPOFF64 18
#define R_X86_64_TLSGD 19
#define R_X86_64_TLSLD 20
#define R_X86_64_DTPOFF32 21
#define R_X86_64_GOTTPOFF 22
#define R_X86_64_TPOFF32 23
#define R_X86_64_PC64 24
#define R_X86_64_GOTOFF64 25
#define R_X86_64_GOTPC32 26
#define R_X86_64_GOT64 27
#define R_X86_64_GOTPCREL64 28
#define R_X86_64_GOTPC64 29
#define R_X86_64_GOTPLT64 30
#define R_X86_64_PLTOFF64 31
#define R_X86_64_SIZE32 32
#define R_X86_64_SIZE64 33
#define R_X86_64_GOTPC32_TLSDESC 34
#define R_X86_64_TLSDESC_CALL 35
#define R_X86_64_TLSDESC 36
#define R_X86_64_IRELATIVE 37
#define R_X86_64_RELATIVE64 38
#define R_X86_64_GOTPCRELX 41
#define R_X86_64_REX_GOTPCRELX 42
#define R_X86_64_NUM 43

/* ---- aarch64 relocations ---- */

#define R_AARCH64_NONE 0
#define R_AARCH64_ABS64 257
#define R_AARCH64_ABS32 258
#define R_AARCH64_ABS16 259
#define R_AARCH64_PREL64 260
#define R_AARCH64_PREL32 261
#define R_AARCH64_PREL16 262
#define R_AARCH64_MOVW_UABS_G0 263
#define R_AARCH64_MOVW_UABS_G0_NC 264
#define R_AARCH64_MOVW_UABS_G1 265
#define R_AARCH64_MOVW_UABS_G1_NC 266
#define R_AARCH64_MOVW_UABS_G2 267
#define R_AARCH64_MOVW_UABS_G2_NC 268
#define R_AARCH64_MOVW_UABS_G3 269
#define R_AARCH64_MOVW_SABS_G0 270
#define R_AARCH64_MOVW_SABS_G1 271
#define R_AARCH64_MOVW_SABS_G2 272
#define R_AARCH64_LD_PREL_LO19 273
#define R_AARCH64_ADR_PREL_LO21 274
#define R_AARCH64_ADR_PREL_PG_HI21 275
#define R_AARCH64_ADR_PREL_PG_HI21_NC 276
#define R_AARCH64_ADD_ABS_LO12_NC 277
#define R_AARCH64_LDST8_ABS_LO12_NC 278
#define R_AARCH64_TSTBR14 279
#define R_AARCH64_CONDBR19 280
#define R_AARCH64_JUMP26 282
#define R_AARCH64_CALL26 283
#define R_AARCH64_LDST16_ABS_LO12_NC 284
#define R_AARCH64_LDST32_ABS_LO12_NC 285
#define R_AARCH64_LDST64_ABS_LO12_NC 286
#define R_AARCH64_MOVW_PREL_G0 287
#define R_AARCH64_MOVW_PREL_G0_NC 288
#define R_AARCH64_MOVW_PREL_G1 289
#define R_AARCH64_MOVW_PREL_G1_NC 290
#define R_AARCH64_MOVW_PREL_G2 291
#define R_AARCH64_MOVW_PREL_G2_NC 292
#define R_AARCH64_MOVW_PREL_G3 293
#define R_AARCH64_LDST128_ABS_LO12_NC 299
#define R_AARCH64_MOVW_GOTOFF_G0 300
#define R_AARCH64_MOVW_GOTOFF_G0_NC 301
#define R_AARCH64_MOVW_GOTOFF_G1 302
#define R_AARCH64_MOVW_GOTOFF_G1_NC 303
#define R_AARCH64_MOVW_GOTOFF_G2 304
#define R_AARCH64_MOVW_GOTOFF_G2_NC 305
#define R_AARCH64_MOVW_GOTOFF_G3 306
#define R_AARCH64_GOTREL64 307
#define R_AARCH64_GOTREL32 308
#define R_AARCH64_GOT_LD_PREL19 309
#define R_AARCH64_LD64_GOTOFF_LO15 310
#define R_AARCH64_ADR_GOT_PAGE 311
#define R_AARCH64_LD64_GOT_LO12_NC 312
#define R_AARCH64_LD64_GOTPAGE_LO15 313
#define R_AARCH64_TLSGD_ADR_PREL21 512
#define R_AARCH64_TLSGD_ADR_PAGE21 513
#define R_AARCH64_TLSGD_ADD_LO12_NC 514
#define R_AARCH64_TLSGD_MOVW_G1 515
#define R_AARCH64_TLSGD_MOVW_G0_NC 516
#define R_AARCH64_TLSLD_ADR_PREL21 517
#define R_AARCH64_TLSLD_ADR_PAGE21 518
#define R_AARCH64_TLSLD_ADD_LO12_NC 519
#define R_AARCH64_TLSIE_MOVW_GOTTPREL_G1 539
#define R_AARCH64_TLSIE_MOVW_GOTTPREL_G0_NC 540
#define R_AARCH64_TLSIE_ADR_GOTTPREL_PAGE21 541
#define R_AARCH64_TLSIE_LD64_GOTTPREL_LO12_NC 542
#define R_AARCH64_TLSIE_LD_GOTTPREL_PREL19 543
#define R_AARCH64_TLSLE_MOVW_TPREL_G2 544
#define R_AARCH64_TLSLE_MOVW_TPREL_G1 545
#define R_AARCH64_TLSLE_MOVW_TPREL_G1_NC 546
#define R_AARCH64_TLSLE_MOVW_TPREL_G0 547
#define R_AARCH64_TLSLE_MOVW_TPREL_G0_NC 548
#define R_AARCH64_TLSLE_ADD_TPREL_HI12 549
#define R_AARCH64_TLSLE_ADD_TPREL_LO12 550
#define R_AARCH64_TLSLE_ADD_TPREL_LO12_NC 551
#define R_AARCH64_TLSLE_LDST8_TPREL_LO12 552
#define R_AARCH64_TLSLE_LDST8_TPREL_LO12_NC 553
#define R_AARCH64_TLSLE_LDST16_TPREL_LO12 554
#define R_AARCH64_TLSLE_LDST16_TPREL_LO12_NC 555
#define R_AARCH64_TLSLE_LDST32_TPREL_LO12 556
#define R_AARCH64_TLSLE_LDST32_TPREL_LO12_NC 557
#define R_AARCH64_TLSLE_LDST64_TPREL_LO12 558
#define R_AARCH64_TLSLE_LDST64_TPREL_LO12_NC 559
#define R_AARCH64_TLSDESC_LD_PREL19 560
#define R_AARCH64_TLSDESC_ADR_PREL21 561
#define R_AARCH64_TLSDESC_ADR_PAGE21 562
#define R_AARCH64_TLSDESC_LD64_LO12 563
#define R_AARCH64_TLSDESC_ADD_LO12 564
#define R_AARCH64_TLSDESC_OFF_G1 565
#define R_AARCH64_TLSDESC_OFF_G0_NC 566
#define R_AARCH64_TLSDESC_LDR 567
#define R_AARCH64_TLSDESC_ADD 568
#define R_AARCH64_TLSDESC_CALL 569
#define R_AARCH64_TLSLE_LDST128_TPREL_LO12 570
#define R_AARCH64_TLSLE_LDST128_TPREL_LO12_NC 571
#define R_AARCH64_TLSLD_LDST128_DTPREL_LO12 572
#define R_AARCH64_TLSLD_LDST128_DTPREL_LO12_NC 573
#define R_AARCH64_COPY 1024
#define R_AARCH64_GLOB_DAT 1025
#define R_AARCH64_JUMP_SLOT 1026
#define R_AARCH64_RELATIVE 1027
#define R_AARCH64_TLS_DTPMOD 1028
#define R_AARCH64_TLS_DTPMOD64 1028
#define R_AARCH64_TLS_DTPREL 1029
#define R_AARCH64_TLS_DTPREL64 1029
#define R_AARCH64_TLS_TPREL 1030
#define R_AARCH64_TLS_TPREL64 1030
#define R_AARCH64_TLSDESC 1031
#define R_AARCH64_IRELATIVE 1032
