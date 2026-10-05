/* addr2line: translate addresses of an ELF64 file into file names, line
 * numbers and function names with the DWARF versions 2 to 5, as the GNU
 * program does.
 *
 *     addr2line [-afhijps] [-e FILE] [-j SECTION] [address...]
 *
 * The program reads .debug_info with .debug_abbrev for the functions and
 * the inlined calls, and the line number program of .debug_line for the
 * file and the line.  A function that DWARF does not describe receives
 * the name of the nearest symbol of the symbol table.  Compressed debug
 * sections are not supported.  An object file receives the relocations of
 * its debug sections before the reading.
 *
 * The reader follows the search of the GNU library BFD: the first unit
 * whose address ranges contain the address and whose line table or
 * function table matches ends the search.  The function with the
 * smallest range that contains the address is the innermost function.
 * The line table row is the last row of the sequence whose address does
 * not exceed the searched address. */
#include "lib/binutils.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- the reader of bytes ---- */

/* A reader walks over a part of a section.  The field err becomes 1 when
 * a read reaches beyond the end. */
struct reader {
    const uint8_t *p, *end;
    int err;
};

static uint64_t rd_uint(struct reader *r, unsigned n)
{
    uint64_t v = 0;
    if (n > 8 || (size_t)(r->end - r->p) < n) {
        r->err = 1;
        r->p = r->end;
        return 0;
    }
    for (unsigned i = 0; i < n; i++)
        v |= (uint64_t)r->p[i] << (8 * i);
    r->p += n;
    return v;
}

static uint64_t rd_uleb(struct reader *r)
{
    uint64_t v = 0;
    unsigned shift = 0;
    while (r->p < r->end) {
        uint8_t b = *r->p++;
        if (shift < 64)
            v |= (uint64_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80))
            return v;
    }
    r->err = 1;
    return 0;
}

static int64_t rd_sleb(struct reader *r)
{
    uint64_t v = 0;
    unsigned shift = 0;
    uint8_t b = 0;
    while (r->p < r->end) {
        b = *r->p++;
        if (shift < 64)
            v |= (uint64_t)(b & 0x7f) << shift;
        shift += 7;
        if (!(b & 0x80)) {
            if (shift < 64 && (b & 0x40))
                v |= ~(uint64_t)0 << shift;
            return (int64_t)v;
        }
    }
    r->err = 1;
    return 0;
}

static void rd_skip(struct reader *r, uint64_t n)
{
    if (n > (uint64_t)(r->end - r->p)) {
        r->err = 1;
        r->p = r->end;
        return;
    }
    r->p += n;
}

/* rd_cstr returns the string at the position, or NULL without a
 * terminator before the end. */
static const char *rd_cstr(struct reader *r)
{
    const char *s = (const char *)r->p;
    size_t n = strnlen(s, (size_t)(r->end - r->p));
    if (r->p + n >= r->end) {
        r->err = 1;
        r->p = r->end;
        return NULL;
    }
    r->p += n + 1;
    return s;
}

/* ---- the debug sections ---- */

enum { S_INFO, S_ABBREV, S_LINE, S_STR, S_LINE_STR, S_RANGES, S_RNGLISTS, S_STR_OFFSETS, S_ADDR, S_COUNT };

static const char *const section_names[S_COUNT] = {
    ".debug_info", ".debug_abbrev", ".debug_line", ".debug_str", ".debug_line_str",
    ".debug_ranges", ".debug_rnglists", ".debug_str_offsets", ".debug_addr",
};

struct dsec {
    const uint8_t *data;
    size_t size;
};

/* A line_ref records that the operand at an offset of .debug_line has a
 * relocation against a symbol of the section shndx.  Object files use the record
 * to tell the sequences of different code sections apart. */
struct line_ref {
    uint64_t offset;
    uint32_t shndx;
};

static struct dsec dsec[S_COUNT];
static struct line_ref *line_refs;
static size_t nline_refs;
static struct elffile elf;
static const char *exe_path = "a.out";
static int is_rel;

static struct reader section_reader(int which, uint64_t off)
{
    struct reader r = { dsec[which].data, dsec[which].data, 0 };
    if (off > dsec[which].size) {
        r.err = 1;
        return r;
    }
    r.p = dsec[which].data + off;
    r.end = dsec[which].data + dsec[which].size;
    return r;
}

/* sec_string returns the string at an offset of a section, or NULL. */
static const char *sec_string(int which, uint64_t off)
{
    if (off >= dsec[which].size)
        return NULL;
    const char *s = (const char *)dsec[which].data + off;
    if (strnlen(s, dsec[which].size - off) == dsec[which].size - off)
        return NULL;
    return s;
}

/* apply_relocs adds the relocations of the section index idx to the copy
 * buf of its contents.  Only the absolute relocations that debug sections
 * use are applied. */
static void apply_relocs(unsigned idx, uint8_t *buf, size_t size, int which)
{
    for (unsigned i = 1; i < elf.nsections; i++) {
        const Elf64_Shdr *rs = &elf.sh[i];
        if (rs->sh_type != SHT_RELA || rs->sh_info != idx)
            continue;
        const Elf64_Shdr *symtab = elffile_linked(&elf, rs);
        size_t nsyms;
        const Elf64_Shdr *strtab;
        const Elf64_Sym *syms = elffile_symbols(&elf, symtab, &nsyms, &strtab);
        const Elf64_Rela *rel = elffile_section_data(&elf, rs);
        if (!syms || !rel)
            continue;
        size_t n = rs->sh_size / sizeof *rel;
        for (size_t k = 0; k < n; k++) {
            uint32_t type = (uint32_t)ELF64_R_TYPE(rel[k].r_info);
            uint64_t sym = ELF64_R_SYM(rel[k].r_info);
            unsigned width = 0;
            if (elf.eh->e_machine == EM_X86_64)
                width = type == R_X86_64_64 ? 8 : (type == R_X86_64_32 || type == R_X86_64_32S) ? 4 : 0;
            else if (elf.eh->e_machine == EM_AARCH64)
                width = type == R_AARCH64_ABS64 ? 8 : type == R_AARCH64_ABS32 ? 4 : 0;
            if (!width || sym >= nsyms || rel[k].r_offset > size || size - rel[k].r_offset < width)
                continue;
            uint64_t v = syms[sym].st_value + (uint64_t)rel[k].r_addend;
            for (unsigned b = 0; b < width; b++)
                buf[rel[k].r_offset + b] = (uint8_t)(v >> (8 * b));
            if (which == S_LINE && width == 8) {
                line_refs = bu_realloc(line_refs, (nline_refs + 1) * sizeof *line_refs);
                line_refs[nline_refs].offset = rel[k].r_offset;
                line_refs[nline_refs].shndx = syms[sym].st_shndx;
                nline_refs++;
            }
        }
    }
}

/* load_sections finds the debug sections.  The result is 1 when a
 * section is compressed. */
static int load_sections(void)
{
    int compressed = 0;
    for (int w = 0; w < S_COUNT; w++) {
        const Elf64_Shdr *s = elffile_find_section(&elf, section_names[w]);
        if (!s || s->sh_type == SHT_NOBITS)
            continue;
        if (s->sh_flags & SHF_COMPRESSED) {
            compressed = 1;
            continue;
        }
        const void *data = elffile_section_data(&elf, s);
        if (!data)
            continue;
        if (is_rel) {
            uint8_t *copy = bu_alloc(s->sh_size ? s->sh_size : 1);
            memcpy(copy, data, s->sh_size);
            apply_relocs(elffile_section_index(&elf, s), copy, s->sh_size, w);
            data = copy;
        }
        dsec[w].data = data;
        dsec[w].size = s->sh_size;
    }
    return compressed;
}

/* ---- the attributes ---- */

enum {
    T_SUBPROGRAM = 0x2e, T_INLINED = 0x1d,
    AT_SIBLING = 0x01, AT_NAME = 0x03, AT_STMT_LIST = 0x10, AT_LOW_PC = 0x11, AT_HIGH_PC = 0x12,
    AT_COMP_DIR = 0x1b, AT_ABSTRACT_ORIGIN = 0x31, AT_SPECIFICATION = 0x47, AT_RANGES = 0x55,
    AT_CALL_FILE = 0x58, AT_CALL_LINE = 0x59, AT_LINKAGE_NAME = 0x6e, AT_STR_OFFSETS_BASE = 0x72,
    AT_ADDR_BASE = 0x73, AT_RNGLISTS_BASE = 0x74, AT_MIPS_LINKAGE_NAME = 0x2007,
    FORM_ADDR = 0x01, FORM_BLOCK2 = 0x03, FORM_BLOCK4 = 0x04, FORM_DATA2 = 0x05, FORM_DATA4 = 0x06,
    FORM_DATA8 = 0x07, FORM_STRING = 0x08, FORM_BLOCK = 0x09, FORM_BLOCK1 = 0x0a, FORM_DATA1 = 0x0b,
    FORM_FLAG = 0x0c, FORM_SDATA = 0x0d, FORM_STRP = 0x0e, FORM_UDATA = 0x0f, FORM_REF_ADDR = 0x10,
    FORM_REF1 = 0x11, FORM_REF2 = 0x12, FORM_REF4 = 0x13, FORM_REF8 = 0x14, FORM_REF_UDATA = 0x15,
    FORM_INDIRECT = 0x16, FORM_SEC_OFFSET = 0x17, FORM_EXPRLOC = 0x18, FORM_FLAG_PRESENT = 0x19,
    FORM_STRX = 0x1a, FORM_ADDRX = 0x1b, FORM_REF_SUP4 = 0x1c, FORM_STRP_SUP = 0x1d, FORM_DATA16 = 0x1e,
    FORM_LINE_STRP = 0x1f, FORM_REF_SIG8 = 0x20, FORM_IMPLICIT_CONST = 0x21, FORM_LOCLISTX = 0x22,
    FORM_RNGLISTX = 0x23, FORM_REF_SUP8 = 0x24, FORM_STRX1 = 0x25, FORM_STRX2 = 0x26, FORM_STRX3 = 0x27,
    FORM_STRX4 = 0x28, FORM_ADDRX1 = 0x29, FORM_ADDRX2 = 0x2a, FORM_ADDRX3 = 0x2b, FORM_ADDRX4 = 0x2c,
    FORM_GNU_ADDR_INDEX = 0x1f01, FORM_GNU_STR_INDEX = 0x1f02, FORM_GNU_REF_ALT = 0x1f20,
    FORM_GNU_STRP_ALT = 0x1f21,
};

/* The class of an attribute value. */
enum { C_NONE, C_CONST, C_ADDR, C_ADDRX, C_STR, C_STRX, C_REF, C_SECOFF, C_RNGX, C_BLOCK };

struct attr {
    uint16_t name;
    uint16_t form;
    uint8_t cls;
    uint64_t val;               /* the constant, address, index or offset */
    const char *str;            /* the string of the class C_STR */
};

struct abbrev_attr {
    uint16_t name, form;
    int64_t implicit;
};

struct abbrev {
    uint64_t code;
    uint16_t tag;
    uint8_t children;
    size_t first, count;        /* the attributes in the table */
};

/* ---- the units ---- */

struct range {
    uint64_t low, high;
};

/* A func is a subprogram or an inlined call.  The field caller points to
 * the index of the enclosing function for an inlined call. */
struct func {
    int tag;
    const char *name;
    struct range *ranges;
    size_t nranges;
    long caller;
    uint64_t call_file;
    uint64_t call_line;
    int has_call_file;
};

struct line_row {
    uint64_t addr;
    uint64_t file;
    uint32_t line;
    uint32_t disc;
    uint8_t end;
};

/* A sequence covers the rows first to first + count - 1.  The last row is
 * the end row.  The field shndx is the section of the first address, or 0
 * when unknown. */
struct line_seq {
    size_t first, count;
    uint64_t low, high;
    uint32_t shndx;
};

struct file_entry {
    const char *name;
    uint64_t dir;
};

struct line_table {
    unsigned version;
    const char **dirs;
    size_t ndirs;
    struct file_entry *files;
    size_t nfiles;
    struct line_row *rows;
    size_t nrows;
    struct line_seq *seqs;
    size_t nseqs;
};

#define MAX_ATTRS 64

struct unit {
    size_t start;               /* the offset of the unit header in .debug_info */
    size_t die_start;
    size_t end;
    unsigned version, addr_size, offset_size, type;
    struct abbrev *abbrevs;
    size_t nabbrevs;
    struct abbrev_attr *aattrs;
    size_t naattrs;
    const char *name, *comp_dir;
    uint64_t base;              /* the low address of the unit DIE */
    uint64_t stmt_list;
    int has_stmt;
    uint64_t str_offsets_base, addr_base, rnglists_base;
    struct range *ranges;
    size_t nranges;
    int prepared;               /* 1 after the tables are read */
    int error;
    struct line_table lines;
    struct func *funcs;
    size_t nfuncs;
};

static struct unit *units;
static size_t nunits;

static void range_add(struct range **v, size_t *n, uint64_t low, uint64_t high)
{
    if (low >= high)
        return;
    *v = bu_realloc(*v, (*n + 1) * sizeof **v);
    (*v)[*n].low = low;
    (*v)[*n].high = high;
    (*n)++;
}

static int range_contains(const struct range *v, size_t n, uint64_t addr)
{
    for (size_t i = 0; i < n; i++)
        if (addr >= v[i].low && addr < v[i].high)
            return 1;
    return 0;
}

/* ---- values of the attributes ---- */

static const char *attr_str(const struct unit *u, const struct attr *a)
{
    if (a->cls == C_STR)
        return a->str;
    if (a->cls != C_STRX)
        return NULL;
    uint64_t pos = u->str_offsets_base + a->val * u->offset_size;
    struct reader r = section_reader(S_STR_OFFSETS, pos);
    uint64_t off = rd_uint(&r, u->offset_size);
    return r.err ? NULL : sec_string(S_STR, off);
}

/* attr_addr stores the address of a value of the class C_ADDR or C_ADDRX.
 * The result is 0 for another class or an invalid index. */
static int attr_addr(const struct unit *u, const struct attr *a, uint64_t *out)
{
    if (a->cls == C_ADDR) {
        *out = a->val;
        return 1;
    }
    if (a->cls != C_ADDRX)
        return 0;
    struct reader r = section_reader(S_ADDR, u->addr_base + a->val * u->addr_size);
    *out = rd_uint(&r, u->addr_size);
    return !r.err;
}

/* attr_read reads one value of the form.  The offset of a reference is
 * relative to the start of .debug_info. */
static int attr_read(const struct unit *u, struct reader *r, unsigned form, int64_t implicit, struct attr *a)
{
    a->form = (uint16_t)form;
    a->cls = C_NONE;
    a->val = 0;
    a->str = NULL;
    switch (form) {
    case FORM_ADDR:
        a->cls = C_ADDR;
        a->val = rd_uint(r, u->addr_size);
        break;
    case FORM_BLOCK2: rd_skip(r, rd_uint(r, 2)); a->cls = C_BLOCK; break;
    case FORM_BLOCK4: rd_skip(r, rd_uint(r, 4)); a->cls = C_BLOCK; break;
    case FORM_BLOCK: case FORM_EXPRLOC: rd_skip(r, rd_uleb(r)); a->cls = C_BLOCK; break;
    case FORM_BLOCK1: rd_skip(r, rd_uint(r, 1)); a->cls = C_BLOCK; break;
    case FORM_DATA1: case FORM_FLAG: a->cls = C_CONST; a->val = rd_uint(r, 1); break;
    case FORM_DATA2: a->cls = C_CONST; a->val = rd_uint(r, 2); break;
    case FORM_DATA4: a->cls = C_CONST; a->val = rd_uint(r, 4); break;
    case FORM_DATA8: a->cls = C_CONST; a->val = rd_uint(r, 8); break;
    case FORM_DATA16: rd_skip(r, 16); a->cls = C_BLOCK; break;
    case FORM_SDATA: a->cls = C_CONST; a->val = (uint64_t)rd_sleb(r); break;
    case FORM_UDATA: a->cls = C_CONST; a->val = rd_uleb(r); break;
    case FORM_IMPLICIT_CONST: a->cls = C_CONST; a->val = (uint64_t)implicit; break;
    case FORM_FLAG_PRESENT: a->cls = C_CONST; a->val = 1; break;
    case FORM_STRING:
        a->cls = C_STR;
        a->str = rd_cstr(r);
        break;
    case FORM_STRP:
        a->cls = C_STR;
        a->str = sec_string(S_STR, rd_uint(r, u->offset_size));
        break;
    case FORM_LINE_STRP:
        a->cls = C_STR;
        a->str = sec_string(S_LINE_STR, rd_uint(r, u->offset_size));
        break;
    case FORM_STRP_SUP: case FORM_GNU_STRP_ALT: case FORM_GNU_REF_ALT:
        rd_skip(r, u->offset_size);
        a->cls = C_BLOCK;
        break;
    case FORM_STRX: case FORM_GNU_STR_INDEX: a->cls = C_STRX; a->val = rd_uleb(r); break;
    case FORM_STRX1: a->cls = C_STRX; a->val = rd_uint(r, 1); break;
    case FORM_STRX2: a->cls = C_STRX; a->val = rd_uint(r, 2); break;
    case FORM_STRX3: a->cls = C_STRX; a->val = rd_uint(r, 3); break;
    case FORM_STRX4: a->cls = C_STRX; a->val = rd_uint(r, 4); break;
    case FORM_ADDRX: case FORM_GNU_ADDR_INDEX: a->cls = C_ADDRX; a->val = rd_uleb(r); break;
    case FORM_ADDRX1: a->cls = C_ADDRX; a->val = rd_uint(r, 1); break;
    case FORM_ADDRX2: a->cls = C_ADDRX; a->val = rd_uint(r, 2); break;
    case FORM_ADDRX3: a->cls = C_ADDRX; a->val = rd_uint(r, 3); break;
    case FORM_ADDRX4: a->cls = C_ADDRX; a->val = rd_uint(r, 4); break;
    case FORM_REF1: a->cls = C_REF; a->val = u->start + rd_uint(r, 1); break;
    case FORM_REF2: a->cls = C_REF; a->val = u->start + rd_uint(r, 2); break;
    case FORM_REF4: a->cls = C_REF; a->val = u->start + rd_uint(r, 4); break;
    case FORM_REF8: a->cls = C_REF; a->val = u->start + rd_uint(r, 8); break;
    case FORM_REF_UDATA: a->cls = C_REF; a->val = u->start + rd_uleb(r); break;
    case FORM_REF_ADDR:
        a->cls = C_REF;
        a->val = rd_uint(r, u->version <= 2 ? u->addr_size : u->offset_size);
        break;
    case FORM_REF_SIG8: rd_skip(r, 8); a->cls = C_BLOCK; break;
    case FORM_REF_SUP4: rd_skip(r, 4); a->cls = C_BLOCK; break;
    case FORM_REF_SUP8: rd_skip(r, 8); a->cls = C_BLOCK; break;
    case FORM_SEC_OFFSET:
        a->cls = C_SECOFF;
        a->val = rd_uint(r, u->offset_size);
        break;
    case FORM_LOCLISTX: rd_uleb(r); a->cls = C_BLOCK; break;
    case FORM_RNGLISTX: a->cls = C_RNGX; a->val = rd_uleb(r); break;
    case FORM_INDIRECT: {
        unsigned real = (unsigned)rd_uleb(r);
        int64_t imp = real == FORM_IMPLICIT_CONST ? rd_sleb(r) : 0;
        if (r->err || real == FORM_INDIRECT)
            return -1;
        return attr_read(u, r, real, imp, a);
    }
    default:
        return -1;
    }
    return r->err ? -1 : 0;
}

/* ---- the abbreviations ---- */

static int abbrev_load(struct unit *u, uint64_t offset)
{
    struct reader r = section_reader(S_ABBREV, offset);
    while (!r.err && r.p < r.end) {
        uint64_t code = rd_uleb(&r);
        if (code == 0)
            break;
        u->abbrevs = bu_realloc(u->abbrevs, (u->nabbrevs + 1) * sizeof *u->abbrevs);
        struct abbrev *ab = &u->abbrevs[u->nabbrevs++];
        ab->code = code;
        ab->tag = (uint16_t)rd_uleb(&r);
        ab->children = (uint8_t)rd_uint(&r, 1);
        ab->first = u->naattrs;
        ab->count = 0;
        for (;;) {
            uint64_t name = rd_uleb(&r), form = rd_uleb(&r);
            int64_t imp = form == FORM_IMPLICIT_CONST ? rd_sleb(&r) : 0;
            if (r.err || (name == 0 && form == 0))
                break;
            u->aattrs = bu_realloc(u->aattrs, (u->naattrs + 1) * sizeof *u->aattrs);
            u->aattrs[u->naattrs].name = (uint16_t)name;
            u->aattrs[u->naattrs].form = (uint16_t)form;
            u->aattrs[u->naattrs].implicit = imp;
            u->naattrs++;
            ab->count++;
        }
    }
    return r.err ? -1 : 0;
}

static const struct abbrev *abbrev_find(const struct unit *u, uint64_t code)
{
    if (code >= 1 && code <= u->nabbrevs && u->abbrevs[code - 1].code == code)
        return &u->abbrevs[code - 1];
    for (size_t i = 0; i < u->nabbrevs; i++)
        if (u->abbrevs[i].code == code)
            return &u->abbrevs[i];
    return NULL;
}

/* die_read reads the DIE at the reader.  The result is the abbreviation,
 * or NULL for the end of a list of children (*null is 1) or for a
 * damaged entry (*null is 0). */
static const struct abbrev *die_read(const struct unit *u, struct reader *r, struct attr *attrs, int *null)
{
    *null = 0;
    uint64_t code = rd_uleb(r);
    if (r->err)
        return NULL;
    if (code == 0) {
        *null = 1;
        return NULL;
    }
    const struct abbrev *ab = abbrev_find(u, code);
    if (!ab || ab->count > MAX_ATTRS)
        return NULL;
    for (size_t i = 0; i < ab->count; i++) {
        const struct abbrev_attr *aa = &u->aattrs[ab->first + i];
        if (attr_read(u, r, aa->form, aa->implicit, &attrs[i]) < 0)
            return NULL;
        attrs[i].name = aa->name;
    }
    return ab;
}

/* ---- the unit headers ---- */

static void units_parse(void)
{
    struct reader r = section_reader(S_INFO, 0);
    while (!r.err && r.p < r.end) {
        const uint8_t *base = dsec[S_INFO].data;
        size_t start = (size_t)(r.p - base);
        uint64_t len = rd_uint(&r, 4);
        unsigned osz = 4;
        if (len == 0xffffffffu) {
            len = rd_uint(&r, 8);
            osz = 8;
        }
        if (r.err || len > (uint64_t)(r.end - r.p))
            break;
        struct reader ur = { r.p, r.p + len, 0 };
        r.p += len;
        struct unit u;
        memset(&u, 0, sizeof u);
        u.start = start;
        u.end = (size_t)(ur.end - base);
        u.offset_size = osz;
        u.version = (unsigned)rd_uint(&ur, 2);
        uint64_t abbrev_off;
        if (u.version >= 5) {
            u.type = (unsigned)rd_uint(&ur, 1);
            u.addr_size = (unsigned)rd_uint(&ur, 1);
            abbrev_off = rd_uint(&ur, osz);
            if (u.type == 4 || u.type == 5)
                rd_skip(&ur, 8);
            else if (u.type == 2 || u.type == 6)
                rd_skip(&ur, 8 + osz);
        } else {
            u.type = 1;
            abbrev_off = rd_uint(&ur, osz);
            u.addr_size = (unsigned)rd_uint(&ur, 1);
        }
        if (ur.err || u.version < 2 || u.version > 5 || u.addr_size == 0 || u.addr_size > 8)
            continue;
        if (u.type == 2 || u.type == 6)
            continue;
        u.die_start = (size_t)(ur.p - base);
        u.str_offsets_base = osz == 8 ? 16 : 8;
        u.addr_base = osz == 8 ? 16 : 8;
        u.rnglists_base = osz == 8 ? 20 : 12;
        u.stmt_list = 0;
        if (abbrev_load(&u, abbrev_off) < 0)
            u.error = 1;
        units = bu_realloc(units, (nunits + 1) * sizeof *units);
        units[nunits++] = u;
    }
}

static struct unit *unit_at(uint64_t offset)
{
    for (size_t i = 0; i < nunits; i++)
        if (offset >= units[i].die_start && offset < units[i].end)
            return &units[i];
    return NULL;
}

/* ---- the lists of ranges ---- */

/* ranges_read stores the ranges that the list at index or offset gives.
 * is_index is 1 for the form rnglistx. */
static void ranges_read(const struct unit *u, uint64_t val, int is_index, struct range **out, size_t *n)
{
    if (u->version < 5) {
        struct reader r = section_reader(S_RANGES, val);
        uint64_t base = u->base;
        uint64_t max = u->addr_size == 8 ? ~(uint64_t)0 : (((uint64_t)1 << (8 * u->addr_size)) - 1);
        while (!r.err && r.p < r.end) {
            uint64_t lo = rd_uint(&r, u->addr_size), hi = rd_uint(&r, u->addr_size);
            if (r.err || (lo == 0 && hi == 0))
                break;
            if (lo == max)
                base = hi;
            else
                range_add(out, n, base + lo, base + hi);
        }
        return;
    }
    uint64_t off = val;
    if (is_index) {
        struct reader t = section_reader(S_RNGLISTS, u->rnglists_base + val * u->offset_size);
        off = u->rnglists_base + rd_uint(&t, u->offset_size);
        if (t.err)
            return;
    }
    struct reader r = section_reader(S_RNGLISTS, off);
    uint64_t base = u->base;
    while (!r.err && r.p < r.end) {
        unsigned kind = (unsigned)rd_uint(&r, 1);
        struct attr ix = { 0, 0, C_ADDRX, 0, NULL };
        uint64_t a, b;
        if (r.err || kind == 0)
            break;
        switch (kind) {
        case 1:
            ix.val = rd_uleb(&r);
            if (attr_addr(u, &ix, &a))
                base = a;
            break;
        case 2: {
            ix.val = rd_uleb(&r);
            struct attr iy = { 0, 0, C_ADDRX, rd_uleb(&r), NULL };
            if (attr_addr(u, &ix, &a) && attr_addr(u, &iy, &b))
                range_add(out, n, a, b);
            break;
        }
        case 3:
            ix.val = rd_uleb(&r);
            b = rd_uleb(&r);
            if (attr_addr(u, &ix, &a))
                range_add(out, n, a, a + b);
            break;
        case 4:
            a = rd_uleb(&r);
            b = rd_uleb(&r);
            range_add(out, n, base + a, base + b);
            break;
        case 5:
            base = rd_uint(&r, u->addr_size);
            break;
        case 6:
            a = rd_uint(&r, u->addr_size);
            b = rd_uint(&r, u->addr_size);
            range_add(out, n, a, b);
            break;
        case 7:
            a = rd_uint(&r, u->addr_size);
            b = rd_uleb(&r);
            range_add(out, n, a, a + b);
            break;
        default:
            return;
        }
    }
}

/* attrs_ranges stores the address ranges that the attributes of a DIE
 * give, from DW_AT_ranges or from DW_AT_low_pc with DW_AT_high_pc. */
static void attrs_ranges(const struct unit *u, const struct attr *attrs, size_t count, struct range **out, size_t *n)
{
    const struct attr *low = NULL, *high = NULL, *ranges = NULL;
    for (size_t i = 0; i < count; i++) {
        if (attrs[i].name == AT_LOW_PC)
            low = &attrs[i];
        else if (attrs[i].name == AT_HIGH_PC)
            high = &attrs[i];
        else if (attrs[i].name == AT_RANGES)
            ranges = &attrs[i];
    }
    if (ranges && (ranges->cls == C_CONST || ranges->cls == C_SECOFF || ranges->cls == C_RNGX)) {
        ranges_read(u, ranges->val, ranges->cls == C_RNGX, out, n);
        return;
    }
    uint64_t lo, hi;
    if (low && high && attr_addr(u, low, &lo)) {
        if (high->cls == C_ADDR || high->cls == C_ADDRX) {
            if (!attr_addr(u, high, &hi))
                return;
        } else {
            hi = lo + high->val;
        }
        range_add(out, n, lo, hi);
    }
}

/* ---- the line number program ---- */

static void table_add_dir(struct line_table *t, const char *dir)
{
    t->dirs = bu_realloc(t->dirs, (t->ndirs + 1) * sizeof *t->dirs);
    t->dirs[t->ndirs++] = dir;
}

static void table_add_file(struct line_table *t, const char *name, uint64_t dir)
{
    t->files = bu_realloc(t->files, (t->nfiles + 1) * sizeof *t->files);
    t->files[t->nfiles].name = name;
    t->files[t->nfiles].dir = dir;
    t->nfiles++;
}

/* entries_read reads a list of directories or files of the version 5
 * header. */
static int entries_read(const struct unit *u, struct reader *r, struct line_table *t, int files)
{
    unsigned nfmt = (unsigned)rd_uint(r, 1);
    uint64_t fmt[32][2];
    if (nfmt > 16)
        return -1;
    for (unsigned i = 0; i < nfmt; i++) {
        fmt[i][0] = rd_uleb(r);
        fmt[i][1] = rd_uleb(r);
    }
    uint64_t count = rd_uleb(r);
    for (uint64_t e = 0; e < count && !r->err; e++) {
        const char *path = NULL;
        uint64_t dir = 0;
        for (unsigned i = 0; i < nfmt; i++) {
            struct attr a;
            if (attr_read(u, r, (unsigned)fmt[i][1], 0, &a) < 0)
                return -1;
            if (fmt[i][0] == 1)
                path = attr_str(u, &a);
            else if (fmt[i][0] == 2)
                dir = a.val;
        }
        if (files)
            table_add_file(t, path, dir);
        else
            table_add_dir(t, path);
    }
    return r->err ? -1 : 0;
}

static void row_add(struct line_table *t, uint64_t addr, uint64_t file, uint64_t line, uint32_t disc, int end)
{
    t->rows = bu_realloc(t->rows, (t->nrows + 1) * sizeof *t->rows);
    struct line_row *row = &t->rows[t->nrows++];
    row->addr = addr;
    row->file = file;
    row->line = (uint32_t)line;
    row->disc = disc;
    row->end = (uint8_t)end;
}

static uint32_t line_ref_section(uint64_t offset)
{
    for (size_t i = 0; i < nline_refs; i++)
        if (line_refs[i].offset == offset)
            return line_refs[i].shndx;
    return 0;
}

/* lines_decode reads the header and runs the program of the unit. */
static int lines_decode(struct unit *u)
{
    struct line_table *t = &u->lines;
    struct reader r = section_reader(S_LINE, u->stmt_list);
    if (r.err)
        return -1;
    uint64_t len = rd_uint(&r, 4);
    unsigned osz = 4;
    if (len == 0xffffffffu) {
        len = rd_uint(&r, 8);
        osz = 8;
    }
    if (r.err || len > (uint64_t)(r.end - r.p))
        return -1;
    r.end = r.p + len;
    t->version = (unsigned)rd_uint(&r, 2);
    if (t->version < 2 || t->version > 5)
        return -1;
    struct unit hdr = *u;
    hdr.offset_size = osz;
    hdr.version = t->version;
    if (t->version >= 5) {
        hdr.addr_size = (unsigned)rd_uint(&r, 1);
        rd_uint(&r, 1);
    }
    uint64_t hlen = rd_uint(&r, osz);
    if (r.err || hlen > (uint64_t)(r.end - r.p))
        return -1;
    const uint8_t *program = r.p + hlen;
    unsigned min_inst = (unsigned)rd_uint(&r, 1);
    unsigned max_ops = t->version >= 4 ? (unsigned)rd_uint(&r, 1) : 1;
    if (max_ops == 0)
        max_ops = 1;
    int default_stmt = (int)rd_uint(&r, 1);
    (void)default_stmt;
    int line_base = (int8_t)rd_uint(&r, 1);
    unsigned line_range = (unsigned)rd_uint(&r, 1);
    unsigned opcode_base = (unsigned)rd_uint(&r, 1);
    uint8_t std_len[256];
    memset(std_len, 0, sizeof std_len);
    for (unsigned i = 1; i < opcode_base; i++)
        std_len[i] = (uint8_t)rd_uint(&r, 1);
    if (r.err || line_range == 0)
        return -1;
    if (t->version >= 5) {
        if (entries_read(&hdr, &r, t, 0) < 0 || entries_read(&hdr, &r, t, 1) < 0)
            return -1;
    } else {
        const char *s;
        while ((s = rd_cstr(&r)) != NULL && *s)
            table_add_dir(t, s);
        while ((s = rd_cstr(&r)) != NULL && *s) {
            uint64_t dir = rd_uleb(&r);
            rd_uleb(&r);
            rd_uleb(&r);
            table_add_file(t, s, dir);
        }
        if (r.err)
            return -1;
    }

    r.p = program;
    uint64_t addr = 0, file = 1, line = 1;
    uint32_t disc = 0;
    unsigned op_index = 0;
    size_t seq_first = 0;
    uint32_t seq_shndx = 0;
    int seq_have_addr = 0;
    while (!r.err && r.p < r.end) {
        unsigned op = (unsigned)rd_uint(&r, 1);
        uint64_t adv = 0;
        int special = 0;
        if (op >= opcode_base) {
            unsigned adj = op - opcode_base;
            line += (uint64_t)(int64_t)(line_base + (int)(adj % line_range));
            adv = adj / line_range;
            special = 1;
        } else if (op == 0) {
            uint64_t elen = rd_uleb(&r);
            if (r.err || elen == 0 || elen > (uint64_t)(r.end - r.p))
                break;
            const uint8_t *next = r.p + elen;
            unsigned sub = (unsigned)rd_uint(&r, 1);
            if (sub == 1) {
                row_add(t, addr, file, line, disc, 1);
                struct line_seq *s;
                t->seqs = bu_realloc(t->seqs, (t->nseqs + 1) * sizeof *t->seqs);
                s = &t->seqs[t->nseqs++];
                s->first = seq_first;
                s->count = t->nrows - seq_first;
                s->low = addr;
                for (size_t i = seq_first; i < t->nrows; i++)
                    if (t->rows[i].addr < s->low)
                        s->low = t->rows[i].addr;
                s->high = addr;
                s->shndx = seq_shndx;
                seq_first = t->nrows;
                addr = 0;
                file = 1;
                line = 1;
                disc = 0;
                op_index = 0;
                seq_have_addr = 0;
                seq_shndx = 0;
            } else if (sub == 2) {
                size_t pos = (size_t)(r.p - dsec[S_LINE].data);
                addr = rd_uint(&r, (unsigned)(elen - 1));
                op_index = 0;
                if (is_rel && !seq_have_addr)
                    seq_shndx = line_ref_section(pos);
                seq_have_addr = 1;
            } else if (sub == 3) {
                const char *name = rd_cstr(&r);
                uint64_t dir = rd_uleb(&r);
                table_add_file(t, name, dir);
            } else if (sub == 4) {
                disc = (uint32_t)rd_uleb(&r);
            }
            r.p = next;
            continue;
        } else {
            switch (op) {
            case 1:
                row_add(t, addr, file, line, disc, 0);
                disc = 0;
                break;
            case 2: adv = rd_uleb(&r); break;
            case 3: line += (uint64_t)rd_sleb(&r); break;
            case 4: file = rd_uleb(&r); break;
            case 5: rd_uleb(&r); break;
            case 8: adv = (255u - opcode_base) / line_range; break;
            case 9:
                addr += rd_uint(&r, 2);
                op_index = 0;
                break;
            case 6: case 7: case 10: case 11:
                break;
            default:
                for (unsigned i = 0; i < std_len[op]; i++)
                    rd_uleb(&r);
                break;
            }
        }
        if (adv) {
            addr += min_inst * ((op_index + adv) / max_ops);
            op_index = (unsigned)((op_index + adv) % max_ops);
        }
        if (special) {
            row_add(t, addr, file, line, disc, 0);
            disc = 0;
        }
    }
    return 0;
}

/* file_name writes the path of the file number idx of the unit. */
static void file_name(const struct unit *u, uint64_t idx, char *out, size_t cap)
{
    const struct line_table *t = &u->lines;
    const struct file_entry *fe = NULL;
    if (t->version >= 5) {
        if (idx < t->nfiles)
            fe = &t->files[idx];
    } else if (idx >= 1 && idx <= t->nfiles) {
        fe = &t->files[idx - 1];
    }
    if (!fe || !fe->name) {
        snprintf(out, cap, "<unknown>");
        return;
    }
    if (fe->name[0] == '/') {
        snprintf(out, cap, "%s", fe->name);
        return;
    }
    const char *sub = NULL;
    if (t->version >= 5) {
        if (fe->dir < t->ndirs)
            sub = t->dirs[fe->dir];
    } else if (fe->dir > 0 && fe->dir <= t->ndirs) {
        sub = t->dirs[fe->dir - 1];
    }
    const char *dir = NULL;
    if (!sub || sub[0] != '/')
        dir = u->comp_dir;
    if (!dir) {
        dir = sub;
        sub = NULL;
    }
    if (!dir)
        snprintf(out, cap, "%s", fe->name);
    else if (sub)
        snprintf(out, cap, "%s/%s/%s", dir, sub, fe->name);
    else
        snprintf(out, cap, "%s/%s", dir, fe->name);
}

/* line_lookup finds the row for the address.  The sequence must belong to
 * the section shndx in an object file. */
static const struct line_row *line_lookup(const struct unit *u, uint64_t addr, unsigned shndx)
{
    const struct line_table *t = &u->lines;
    for (size_t s = 0; s < t->nseqs; s++) {
        const struct line_seq *seq = &t->seqs[s];
        if (addr < seq->low || addr >= seq->high)
            continue;
        if (is_rel && seq->shndx && seq->shndx != shndx)
            continue;
        for (size_t i = seq->first + seq->count - 1; i-- > seq->first;)
            if (t->rows[i].addr <= addr)
                return &t->rows[i];
    }
    return NULL;
}

/* ---- the functions ---- */

/* die_name returns the name of the DIE at the offset.  A linkage name takes
 * precedence over the name.  The attributes DW_AT_abstract_origin and
 * DW_AT_specification supply a missing name. */
static const char *die_name(uint64_t off, int depth)
{
    struct unit *u = unit_at(off);
    if (!u || depth > 8)
        return NULL;
    struct reader r = section_reader(S_INFO, off);
    struct attr attrs[MAX_ATTRS];
    int null;
    const struct abbrev *ab = die_read(u, &r, attrs, &null);
    if (!ab)
        return NULL;
    const char *linkage = NULL, *name = NULL;
    uint64_t ref = 0;
    int has_ref = 0;
    for (size_t i = 0; i < ab->count; i++) {
        switch (attrs[i].name) {
        case AT_LINKAGE_NAME: case AT_MIPS_LINKAGE_NAME: linkage = attr_str(u, &attrs[i]); break;
        case AT_NAME: name = attr_str(u, &attrs[i]); break;
        case AT_ABSTRACT_ORIGIN: case AT_SPECIFICATION:
            if (attrs[i].cls == C_REF) {
                ref = attrs[i].val;
                has_ref = 1;
            }
            break;
        default: break;
        }
    }
    if (linkage)
        return linkage;
    if (name)
        return name;
    return has_ref ? die_name(ref, depth + 1) : NULL;
}

/* unit_scan reads the unit DIE and the functions below it. */
static int unit_scan(struct unit *u)
{
    struct reader r = section_reader(S_INFO, u->die_start);
    r.end = dsec[S_INFO].data + u->end;
    struct attr attrs[MAX_ATTRS];
    int null;
    const struct abbrev *ab = die_read(u, &r, attrs, &null);
    if (!ab)
        return -1;
    for (size_t i = 0; i < ab->count; i++) {
        struct attr *a = &attrs[i];
        if (a->cls != C_CONST && a->cls != C_SECOFF)
            continue;
        if (a->name == AT_STMT_LIST) {
            u->stmt_list = a->val;
            u->has_stmt = 1;
        } else if (a->name == AT_STR_OFFSETS_BASE) {
            u->str_offsets_base = a->val;
        } else if (a->name == AT_ADDR_BASE) {
            u->addr_base = a->val;
        } else if (a->name == AT_RNGLISTS_BASE) {
            u->rnglists_base = a->val;
        }
    }
    for (size_t i = 0; i < ab->count; i++) {
        uint64_t v;
        if (attrs[i].name == AT_LOW_PC && attr_addr(u, &attrs[i], &v))
            u->base = v;
        else if (attrs[i].name == AT_NAME)
            u->name = attr_str(u, &attrs[i]);
        else if (attrs[i].name == AT_COMP_DIR)
            u->comp_dir = attr_str(u, &attrs[i]);
    }
    attrs_ranges(u, attrs, ab->count, &u->ranges, &u->nranges);
    if (u->has_stmt && lines_decode(u) < 0)
        return -1;
    if (!ab->children)
        return 0;

    /* ctx[d] is the index of the function that encloses the children at depth d, or -1. */
    long ctx_small[64];
    long *ctx = ctx_small;
    size_t ctx_cap = 64, depth = 1;
    ctx[1] = -1;
    while (!r.err && r.p < r.end && depth > 0) {
        ab = die_read(u, &r, attrs, &null);
        if (!ab) {
            if (!null)
                break;
            depth--;
            continue;
        }
        long enclosing = ctx[depth], self = -1;
        if (ab->tag == T_SUBPROGRAM || ab->tag == T_INLINED) {
            u->funcs = bu_realloc(u->funcs, (u->nfuncs + 1) * sizeof *u->funcs);
            struct func *f = &u->funcs[u->nfuncs];
            memset(f, 0, sizeof *f);
            self = (long)u->nfuncs++;
            f->tag = ab->tag;
            f->caller = ab->tag == T_INLINED ? enclosing : -1;
            attrs_ranges(u, attrs, ab->count, &f->ranges, &f->nranges);
            const char *linkage = NULL, *name = NULL;
            uint64_t ref = 0;
            int has_ref = 0;
            for (size_t i = 0; i < ab->count; i++) {
                switch (attrs[i].name) {
                case AT_LINKAGE_NAME: case AT_MIPS_LINKAGE_NAME: linkage = attr_str(u, &attrs[i]); break;
                case AT_NAME: name = attr_str(u, &attrs[i]); break;
                case AT_ABSTRACT_ORIGIN: case AT_SPECIFICATION:
                    if (attrs[i].cls == C_REF && !has_ref) {
                        ref = attrs[i].val;
                        has_ref = 1;
                    }
                    break;
                case AT_CALL_FILE:
                    f->call_file = attrs[i].val;
                    f->has_call_file = 1;
                    break;
                case AT_CALL_LINE: f->call_line = attrs[i].val; break;
                default: break;
                }
            }
            f->name = linkage ? linkage : name;
            if (!f->name && has_ref)
                f->name = die_name(ref, 0);
        }
        if (ab->children) {
            depth++;
            if (depth >= ctx_cap) {
                ctx_cap *= 2;
                if (ctx == ctx_small) {
                    ctx = bu_alloc(ctx_cap * sizeof *ctx);
                    memcpy(ctx, ctx_small, sizeof ctx_small);
                } else {
                    ctx = bu_realloc(ctx, ctx_cap * sizeof *ctx);
                }
            }
            ctx[depth] = self >= 0 ? self : enclosing;
        }
    }
    if (ctx != ctx_small)
        free(ctx);
    return 0;
}

/* unit_prepare reads the line table and the functions once.  The result
 * is 0 for a usable unit. */
static int unit_prepare(struct unit *u)
{
    if (u->error)
        return -1;
    if (!u->prepared) {
        u->prepared = 1;
        if (unit_scan(u) < 0)
            u->error = 1;
    }
    return u->error ? -1 : 0;
}

/* The symbol table is read before the units are searched. */
static const Elf64_Sym *symbols;
static size_t nsymbols;
static const Elf64_Shdr *symstrtab;

/* func_in_section reports whether the function belongs to the code
 * section shndx of an object file.  An object file has the same
 * addresses in every code section.  The section of a subprogram is the
 * section of a symbol that has the name and the low address of the
 * subprogram.  An
 * inlined call belongs to the section of its enclosing subprogram. */
static int func_in_section(const struct unit *u, const struct func *f, unsigned shndx)
{
    while (f->tag == T_INLINED && f->caller >= 0)
        f = &u->funcs[f->caller];
    for (size_t k = 0; k < f->nranges; k++)
        for (size_t i = 1; i < nsymbols; i++)
            if (symbols[i].st_shndx == shndx && symbols[i].st_value == f->ranges[k].low &&
                ELF64_ST_TYPE(symbols[i].st_info) != STT_SECTION && ELF64_ST_TYPE(symbols[i].st_info) != STT_FILE) {
                const char *name = elffile_string(&elf, symstrtab, symbols[i].st_name);
                if (!f->name || (name && strcmp(name, f->name) == 0))
                    return 1;
            }
    return 0;
}

/* func_lookup returns the index of the function with the smallest range
 * that contains the address, or -1.  On equal sizes the function that the
 * unit lists last is selected. */
static long func_lookup(const struct unit *u, uint64_t addr, unsigned shndx)
{
    long best = -1;
    uint64_t best_size = 0;
    for (size_t i = u->nfuncs; i-- > 0;) {
        const struct func *f = &u->funcs[i];
        for (size_t k = 0; k < f->nranges; k++) {
            uint64_t lo = f->ranges[k].low, hi = f->ranges[k].high;
            if (addr >= lo && addr < hi && (best < 0 || hi - lo < best_size) &&
                (!is_rel || !symbols || func_in_section(u, f, shndx))) {
                best = (long)i;
                best_size = hi - lo;
            }
        }
    }
    return best;
}

/* ---- the symbol table ---- */

/* sym_is_mapping reports whether the symbol is an AArch64 mapping symbol
 * such as $x or $d.  The search ignores these symbols. */
static int sym_is_mapping(const Elf64_Sym *q)
{
    if (elf.eh->e_machine != EM_AARCH64)
        return 0;
    const char *name = elffile_string(&elf, symstrtab, q->st_name);
    return name && name[0] == '$' && (name[1] == 'x' || name[1] == 'd') && (name[2] == '\0' || name[2] == '.');
}

/* sym_covers reports whether the size of the symbol covers the address.
 * A symbol without size covers its own address. */
static int sym_covers(const Elf64_Sym *q, uint64_t addr)
{
    return addr < q->st_value + q->st_size || (q->st_size == 0 && addr == q->st_value);
}

/* sym_better reports whether the symbol n replaces the symbol c of the
 * same value.  A symbol that covers the address replaces a symbol that
 * does not.  When both symbols cover the address, a function symbol
 * replaces a symbol without type, and the smaller size replaces the
 * larger size.  When neither symbol covers the address, the larger size
 * replaces the smaller size. */
static int sym_better(const Elf64_Sym *n, const Elf64_Sym *c, uint64_t addr)
{
    int cn = sym_covers(n, addr), cc = sym_covers(c, addr);
    if (cn != cc)
        return cn;
    if (cn) {
        int fn = ELF64_ST_TYPE(n->st_info) != STT_NOTYPE, fc = ELF64_ST_TYPE(c->st_info) != STT_NOTYPE;
        if (fn != fc)
            return fn;
        return n->st_size < c->st_size;
    }
    return n->st_size > c->st_size;
}

/* sym_find follows the search of the GNU library BFD for a function: the
 * symbol of the section with the greatest value that does not exceed the
 * address, and the name of the file symbol that precedes it.  It stores
 * NULL for an unknown name. */
static int sym_find(unsigned shndx, uint64_t vma, uint64_t addr, const char **file, const char **func)
{
    enum { NOTHING, SYMBOL, FILE_AFTER } state = NOTHING;
    const char *filename = NULL, *fname = NULL;
    const Elf64_Sym *best = NULL, *last_file = NULL;
    for (size_t i = 1; i < nsymbols; i++) {
        const Elf64_Sym *q = &symbols[i];
        unsigned type = ELF64_ST_TYPE(q->st_info);
        if (type == STT_FILE) {
            last_file = q;
            if (state == SYMBOL)
                state = FILE_AFTER;
            continue;
        }
        if (type == STT_NOTYPE || type == STT_FUNC || type == STT_GNU_IFUNC) {
            if (sym_is_mapping(q))
                continue;
            if (q->st_shndx == shndx && q->st_value >= vma && q->st_value <= addr &&
                (!best || q->st_value > best->st_value ||
                 (q->st_value == best->st_value && sym_better(q, best, addr)))) {
                best = q;
                filename = NULL;
                if (last_file && (ELF64_ST_BIND(q->st_info) == STB_LOCAL || state != FILE_AFTER))
                    filename = elffile_string(&elf, symstrtab, last_file->st_name);
            }
        }
        if (state == NOTHING)
            state = SYMBOL;
    }
    if (!best)
        return 0;
    fname = elffile_string(&elf, symstrtab, best->st_name);
    *file = filename;
    *func = fname;
    return 1;
}

/* ---- the search ---- */

#define PATH_CAP 4096

struct result {
    const char *func;           /* the name of the function, or NULL */
    int has_file;
    char file[PATH_CAP];
    unsigned line;
    unsigned disc;
    const struct unit *unit;
    long chain;                 /* the inlined function whose callers follow, or -1 */
};

/* unit_lookup searches one unit for the address. */
static int unit_lookup(struct unit *u, uint64_t addr, unsigned shndx, struct result *res)
{
    long fi = func_lookup(u, addr, shndx);
    const struct line_row *row = line_lookup(u, addr, shndx);
    res->unit = u;
    res->func = NULL;
    res->chain = -1;
    res->has_file = 0;
    res->line = 0;
    res->disc = 0;
    if (fi >= 0) {
        res->func = u->funcs[fi].name;
        if (u->funcs[fi].tag == T_INLINED)
            res->chain = fi;
    }
    if (row) {
        file_name(u, row->file, res->file, sizeof res->file);
        res->has_file = 1;
        res->line = row->line;
        res->disc = row->disc;
    }
    return row != NULL || fi >= 0;
}

static int find_nearest(unsigned shndx, uint64_t vma, uint64_t offset, struct result *res)
{
    uint64_t addr = vma + offset;
    int found = 0;
    for (size_t i = 0; i < nunits && !found; i++) {
        struct unit *u = &units[i];
        if (unit_prepare(u) < 0)
            continue;
        if (u->nranges == 0 || range_contains(u->ranges, u->nranges, addr))
            found = unit_lookup(u, addr, shndx, res);
    }
    const char *sfile = NULL, *sfunc = NULL;
    int sym = symbols && sym_find(shndx, vma, addr, &sfile, &sfunc);
    if (found) {
        if (!res->func && sym) {
            res->func = sfunc;
            if (!res->has_file && sfile) {
                snprintf(res->file, sizeof res->file, "%s", sfile);
                res->has_file = 1;
            }
        }
        return 1;
    }
    if (!sym)
        return 0;
    res->func = sfunc;
    res->has_file = sfile != NULL;
    if (sfile)
        snprintf(res->file, sizeof res->file, "%s", sfile);
    res->line = 0;
    res->disc = 0;
    res->chain = -1;
    res->unit = NULL;
    return 1;
}

/* ---- the output ---- */

static int show_addresses, show_functions, show_basenames, show_inlines, pretty;
static const char *section_name;
static unsigned section_index;

static void print_file_line(const char *file, unsigned line, unsigned disc)
{
    if (show_basenames && file) {
        const char *slash = strrchr(file, '/');
        if (slash)
            file = slash + 1;
    }
    printf("%s:", file ? file : "??");
    if (line == 0)
        printf("?\n");
    else if (disc)
        printf("%u (discriminator %u)\n", line, disc);
    else
        printf("%u\n", line);
}

static void print_result(struct result *res)
{
    const char *func = res->func;
    char buf[PATH_CAP];
    const char *file = res->has_file ? res->file : NULL;
    unsigned line = res->line;
    long chain = res->chain;
    for (;;) {
        if (show_functions) {
            printf("%s", func && *func ? func : "??");
            printf(pretty ? " at " : "\n");
        }
        print_file_line(file, line, res->disc);
        if (!show_inlines || chain < 0)
            break;
        const struct func *f = &res->unit->funcs[chain];
        if (f->caller < 0)
            break;
        const struct func *caller = &res->unit->funcs[f->caller];
        func = caller->name;
        if (f->has_call_file) {
            file_name(res->unit, f->call_file, buf, sizeof buf);
            file = buf;
        } else {
            file = NULL;
        }
        line = (unsigned)f->call_line;
        chain = f->caller;
        if (pretty)
            printf(" (inlined by) ");
    }
}

static void translate(const char *text)
{
    char *end;
    uint64_t pc = strtoull(text, &end, 16);
    struct result res;
    int found = 0;
    memset(&res, 0, sizeof res);
    res.chain = -1;
    if (show_addresses) {
        printf("0x%016llx", (unsigned long long)pc);
        printf(pretty ? ": " : "\n");
    }
    if (section_name) {
        uint64_t vma = is_rel ? 0 : elf.sh[section_index].sh_addr;
        if (pc < elf.sh[section_index].sh_size)
            found = find_nearest(section_index, vma, pc, &res);
    } else {
        for (unsigned i = 1; i < elf.nsections && !found; i++) {
            const Elf64_Shdr *s = &elf.sh[i];
            uint64_t vma = is_rel ? 0 : s->sh_addr;
            if (!(s->sh_flags & SHF_ALLOC) || pc < vma || pc - vma >= s->sh_size)
                continue;
            found = find_nearest(i, vma, pc - vma, &res);
        }
    }
    if (found) {
        print_result(&res);
        return;
    }
    if (show_functions)
        printf("%s%s", "??", pretty ? " " : "\n");
    printf("??:0\n");
}

/* ---- the command line ---- */

static void usage(FILE *out)
{
    fprintf(out, "Usage: addr2line [option(s)] [addr(s)]\n"
                 " Convert addresses into line number/file name pairs.\n"
                 " If no addresses are specified on the command line, they will be read from stdin\n"
                 " The options are:\n"
                 "  -a --addresses         Show addresses\n"
                 "  -e --exe=<executable>  Set the input file name (default is a.out)\n"
                 "  -i --inlines           Unwind inlined functions\n"
                 "  -j --section=<name>    Read section-relative offsets instead of addresses\n"
                 "  -p --pretty-print      Make the output easier to read for humans\n"
                 "  -s --basenames         Strip directory names\n"
                 "  -f --functions         Show function names\n"
                 "  -h --help              Display this information\n");
}

int main(int argc, char **argv)
{
    bu_program = "addr2line";
    const char **addrs = bu_alloc((size_t)argc * sizeof *addrs);
    int naddrs = 0, opts_done = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (opts_done || a[0] != '-' || a[1] == '\0') {
            addrs[naddrs++] = a;
            continue;
        }
        if (strcmp(a, "--") == 0) {
            opts_done = 1;
            continue;
        }
        if (a[1] == '-') {
            const char *name = a + 2, *eq = strchr(name, '=');
            size_t n = eq ? (size_t)(eq - name) : strlen(name);
            const char *value = eq ? eq + 1 : NULL;
            if (n && strncmp("addresses", name, n) == 0 && n == 9)
                show_addresses = 1;
            else if (n && strncmp("functions", name, n) == 0 && n == 9)
                show_functions = 1;
            else if (n && strncmp("basenames", name, n) == 0 && n == 9)
                show_basenames = 1;
            else if (n && strncmp("inlines", name, n) == 0 && n == 7)
                show_inlines = 1;
            else if (n && strncmp("pretty-print", name, n) == 0 && n == 12)
                pretty = 1;
            else if (n && strncmp("help", name, n) == 0 && n == 4) {
                usage(stdout);
                return 0;
            } else if ((n == 3 && strncmp("exe", name, n) == 0) || (n == 7 && strncmp("section", name, n) == 0)) {
                if (!value && i + 1 < argc)
                    value = argv[++i];
                if (!value) {
                    usage(stderr);
                    return 1;
                }
                if (n == 3)
                    exe_path = value;
                else
                    section_name = value;
            } else {
                bu_error(NULL, "unrecognized option '%s'", a);
                usage(stderr);
                return 1;
            }
            continue;
        }
        for (const char *p = a + 1; *p; p++) {
            switch (*p) {
            case 'a': show_addresses = 1; break;
            case 'f': show_functions = 1; break;
            case 's': show_basenames = 1; break;
            case 'i': show_inlines = 1; break;
            case 'p': pretty = 1; break;
            case 'h': usage(stdout); return 0;
            case 'e': case 'j': {
                const char *value = p[1] ? p + 1 : (i + 1 < argc ? argv[++i] : NULL);
                if (!value) {
                    bu_error(NULL, "option requires an argument -- '%c'", *p);
                    usage(stderr);
                    return 1;
                }
                if (*p == 'e')
                    exe_path = value;
                else
                    section_name = value;
                p += strlen(p) - 1;
                break;
            }
            default:
                bu_error(NULL, "invalid option -- '%c'", *p);
                usage(stderr);
                return 1;
            }
        }
    }

    void *buf;
    int r = elffile_load(&elf, exe_path, &buf);
    if (r == -ENOENT)
        bu_fatal(NULL, "'%s': No such file", exe_path);
    if (r == -ENOEXEC || r == -EINVAL)
        bu_fatal(exe_path, "file format not recognized");
    if (r < 0)
        bu_fatal(exe_path, "%s", strerror(-r));
    is_rel = elf.eh->e_type == ET_REL;
    if (section_name) {
        const Elf64_Shdr *s = elffile_find_section(&elf, section_name);
        if (!s)
            bu_fatal(exe_path, "cannot find section %s", section_name);
        section_index = elffile_section_index(&elf, s);
    }
    const Elf64_Shdr *symtab = elffile_find_type(&elf, SHT_SYMTAB);
    if (symtab)
        symbols = elffile_symbols(&elf, symtab, &nsymbols, &symstrtab);
    if (load_sections())
        bu_error(exe_path, "compressed debug sections are not supported");
    units_parse();

    if (naddrs > 0) {
        for (int i = 0; i < naddrs; i++)
            translate(addrs[i]);
    } else {
        char line[1024];
        while (fgets(line, sizeof line, stdin)) {
            char *nl = strchr(line, '\n');
            if (nl)
                *nl = '\0';
            translate(line);
            fflush(stdout);
        }
    }
    return 0;
}
