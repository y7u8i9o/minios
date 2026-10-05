/* The ELF rewriter of objcopy and strip (elfrewrite.h).
 *
 * The rewriter reads the sections of the input into a working list and
 * decides which sections and symbols remain.  The symbol table, its string
 * table and the section name table are always built again.  They follow
 * the other sections in this order, as the GNU linker and objcopy place
 * them.  A relocation section of a relocatable object belongs to its
 * target section.  It remains when the target remains, and it receives the
 * new symbol indices.
 *
 * The string tables merge identical strings and strings that end like
 * another string, as the GNU tools do.  Their sizes therefore agree with
 * the sizes in the output of the GNU tools. */
#include "elfrewrite.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>

#define SYMBOL_DROPPED 0xffffffffu

static char message[256];

const char *elfrewrite_message(void)
{
    return message;
}

static int fail(int err, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(message, sizeof message, fmt, ap);
    va_end(ap);
    return err;
}

/* ---- options ---- */

static void list_add(struct elfrewrite_list *l, const char *s)
{
    l->items = bu_realloc(l->items, (l->count + 1) * sizeof *l->items);
    l->items[l->count++] = s;
}

/* A pattern consists of characters, ? for one character and * for any
 * number of characters. */
static int wildcard_match(const char *pattern, const char *s)
{
    if (*pattern == '\0')
        return *s == '\0';
    if (*pattern == '*')
        return wildcard_match(pattern + 1, s) || (*s && wildcard_match(pattern, s + 1));
    if (*s && (*pattern == '?' || *pattern == *s))
        return wildcard_match(pattern + 1, s + 1);
    return 0;
}

static int list_match(const struct elfrewrite_list *l, const char *s)
{
    for (size_t i = 0; i < l->count; i++)
        if (wildcard_match(l->items[i], s))
            return 1;
    return 0;
}

static int add_section_option(struct elfrewrite_options *o, const char *spec)
{
    const char *eq = strchr(spec, '=');
    if (!eq || eq == spec) {
        bu_error(NULL, "bad format for --add-section: '%s'", spec);
        return -1;
    }
    uint8_t *data;
    size_t size;
    int r = bu_read_file(eq + 1, &data, &size);
    if (r < 0) {
        bu_error(NULL, "can't open '%s': %s", eq + 1, strerror(-r));
        return -1;
    }
    o->added = bu_realloc(o->added, (o->added_count + 1) * sizeof *o->added);
    o->added[o->added_count].section = bu_strndup(spec, (size_t)(eq - spec));
    o->added[o->added_count].data = data;
    o->added[o->added_count].size = size;
    o->added_count++;
    return 0;
}

int elfrewrite_apply(struct elfrewrite_options *o, int code, const char *arg)
{
    switch (code) {
    case ELFREWRITE_OPT_STRIP_ALL: o->strip = ELFREWRITE_STRIP_ALL; break;
    case ELFREWRITE_OPT_STRIP_DEBUG: o->strip = ELFREWRITE_STRIP_DEBUG; break;
    case ELFREWRITE_OPT_STRIP_UNNEEDED: o->strip = ELFREWRITE_STRIP_UNNEEDED; break;
    case ELFREWRITE_OPT_PRESERVE_DATES: o->preserve_dates = 1; break;
    case ELFREWRITE_OPT_REMOVE_SECTION: list_add(&o->remove_sections, arg); break;
    case ELFREWRITE_OPT_ONLY_SECTION: list_add(&o->only_sections, arg); break;
    case ELFREWRITE_OPT_STRIP_SYMBOL: list_add(&o->strip_symbols, arg); break;
    case ELFREWRITE_OPT_RETAIN_SYMBOL: list_add(&o->retain_symbols, arg); break;
    case ELFREWRITE_OPT_ADD_SECTION: return add_section_option(o, arg);
    case ELFREWRITE_OPT_OUTPUT: o->output = arg; break;
    case ELFREWRITE_OPT_INPUT_FORMAT: o->input_format = arg; break;
    case ELFREWRITE_OPT_OUTPUT_FORMAT: o->output_format = arg; break;
    default: return -1;
    }
    return 0;
}

/* ---- string tables ---- */

/* A string table builder.  Strings that end like another string share its
 * bytes.  The empty string has the offset 0. */
struct strtab {
    const char **str;
    size_t *length;
    uint32_t *offset;
    size_t count, cap;
    uint8_t *data;
    size_t size;
};

static struct strtab *sort_table;

static size_t strtab_add(struct strtab *t, const char *s)
{
    if (t->count == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 64;
        t->str = bu_realloc(t->str, t->cap * sizeof *t->str);
        t->length = bu_realloc(t->length, t->cap * sizeof *t->length);
        t->offset = bu_realloc(t->offset, t->cap * sizeof *t->offset);
    }
    t->str[t->count] = s;
    t->length[t->count] = strlen(s);
    return t->count++;
}

/* Compare two strings from their last characters. A string that is a
 * suffix of the other string comes first. */
static int compare_reversed(const void *pa, const void *pb)
{
    size_t a = *(const size_t *)pa, b = *(const size_t *)pb;
    const char *sa = sort_table->str[a], *sb = sort_table->str[b];
    size_t la = sort_table->length[a], lb = sort_table->length[b];
    while (la > 0 && lb > 0) {
        unsigned char ca = (unsigned char)sa[--la], cb = (unsigned char)sb[--lb];
        if (ca != cb)
            return ca < cb ? -1 : 1;
    }
    if (la != lb)
        return la < lb ? -1 : 1;
    return 0;
}

static void strtab_finish(struct strtab *t)
{
    size_t n = t->count, m = 0;
    size_t *order = bu_alloc((n ? n : 1) * sizeof *order);
    for (size_t i = 0; i < n; i++) {
        t->offset[i] = 0;
        if (t->length[i] > 0)
            order[m++] = i;
    }
    sort_table = t;
    qsort(order, m, sizeof *order, compare_reversed);
    /* A string is a suffix of its successor in the order, or it is a
     * suffix of no string.  The strings that are no suffix receive space. */
    size_t *target = bu_alloc((m ? m : 1) * sizeof *target);
    size_t size = 1;
    for (size_t k = m; k-- > 0;) {
        target[k] = k;
        if (k + 1 < m) {
            size_t a = order[k], b = order[k + 1];
            size_t la = t->length[a], lb = t->length[b];
            if (la <= lb && memcmp(t->str[a], t->str[b] + lb - la, la) == 0)
                target[k] = target[k + 1];
        }
    }
    for (size_t k = 0; k < m; k++) {
        if (target[k] == k) {
            t->offset[order[k]] = (uint32_t)size;
            size += t->length[order[k]] + 1;
        }
    }
    t->data = bu_alloc(size);
    t->data[0] = '\0';
    t->size = size;
    for (size_t k = 0; k < m; k++) {
        size_t a = order[k];
        if (target[k] == k) {
            memcpy(t->data + t->offset[a], t->str[a], t->length[a] + 1);
        } else {
            size_t b = order[target[k]];
            t->offset[a] = t->offset[b] + (uint32_t)(t->length[b] - t->length[a]);
        }
    }
    free(order);
    free(target);
}

static void strtab_free(struct strtab *t)
{
    free(t->str);
    free(t->length);
    free(t->offset);
    free(t->data);
}

/* ---- the rewriter ---- */

struct section {
    Elf64_Shdr sh;
    const char *name;
    const uint8_t *data;        /* the input contents, NULL for NOBITS */
    uint8_t *owned;             /* contents that the rewriter built, or NULL */
    int retain;
    int special;                /* built again: symbol table, string tables */
    int attached;               /* relocation section that follows its target */
    unsigned index;             /* the index in the output */
    size_t name_id;
};

struct output {
    uint8_t *data;
    size_t size, cap;
};

static void output_reserve(struct output *out, size_t size)
{
    if (size <= out->cap)
        return;
    size_t cap = out->cap ? out->cap : 4096;
    while (cap < size)
        cap *= 2;
    out->data = bu_realloc(out->data, cap);
    memset(out->data + out->cap, 0, cap - out->cap);
    out->cap = cap;
}

static void output_put(struct output *out, size_t off, const void *data, size_t size)
{
    output_reserve(out, off + size);
    if (size)
        memcpy(out->data + off, data, size);
    if (off + size > out->size)
        out->size = off + size;
}

static int is_debug_name(const char *name)
{
    return strncmp(name, ".debug", 6) == 0 || strncmp(name, ".zdebug", 7) == 0 ||
           strncmp(name, ".stab", 5) == 0 || strncmp(name, ".gnu.linkonce.wi.", 17) == 0 ||
           strcmp(name, ".line") == 0;
}

static uint64_t align_up(uint64_t value, uint64_t align)
{
    if (align <= 1)
        return value;
    return (value + align - 1) / align * align;
}

static int symbol_is_local(const Elf64_Sym *s)
{
    return ELF64_ST_BIND(s->st_info) == STB_LOCAL;
}

/* symbol_is_mapping reports whether the symbol is an AArch64 mapping
 * symbol: $x or $d, optionally followed by a dot and a suffix. */
static int symbol_is_mapping(const char *name)
{
    return (name[0] == '$') && (name[1] == 'x' || name[1] == 'd') && (name[2] == '\0' || name[2] == '.');
}

/* symbol_retained reports whether the rules of the strip mode retain one
 * symbol that no relocation uses.  The section symbols of an executable follow
 * symbols_select. */
static int symbol_retained(const struct elfrewrite_options *o, int relocatable, const Elf64_Sym *s,
                            const char *name)
{
    unsigned type = ELF64_ST_TYPE(s->st_info);
    switch (o->strip) {
    case ELFREWRITE_STRIP_DEBUG:
        if (type == STT_FILE)
            return 0;
        return type != STT_SECTION || !relocatable;
    case ELFREWRITE_STRIP_UNNEEDED:
        if (!relocatable)
            return 0;
        if (symbol_is_local(s))
            return type != STT_SECTION && type != STT_FILE && symbol_is_mapping(name);
        return s->st_shndx != SHN_UNDEF;
    case ELFREWRITE_STRIP_ALL:
        return 0;
    default:
        return 1;
    }
}

struct rewrite {
    const struct elffile *f;
    const struct elfrewrite_options *o;
    struct section *sec;
    unsigned nin;
    unsigned symtab_old, strtab_old, shstr_old;
    int relocatable;
    Elf64_Sym *syms;
    size_t nsym;
    const char **symname;
    uint32_t *symmap;
    size_t nlocal, nretained;       /* retained symbols without the null symbol */
    size_t nadded_syms;         /* section symbols of the added sections */
    size_t added_base;          /* the index of the first of these symbols */
};

static int section_load(struct rewrite *r)
{
    const struct elffile *f = r->f;
    for (unsigned i = 0; i < r->nin; i++) {
        struct section *s = &r->sec[i];
        s->sh = f->sh[i];
        s->name = elffile_section_name(f, &f->sh[i]);
        if (i == 0)
            continue;
        if (s->sh.sh_type == SHT_SYMTAB_SHNDX)
            return fail(-ENOTSUP, "sections with extended indices are not supported");
        if (s->sh.sh_type != SHT_NOBITS) {
            s->data = elffile_section_data(f, &f->sh[i]);
            if (!s->data && s->sh.sh_size > 0)
                return fail(-EINVAL, "section '%s' lies outside the file", s->name);
        }
        if (s->sh.sh_type == SHT_SYMTAB && !r->symtab_old) {
            r->symtab_old = i;
            r->strtab_old = s->sh.sh_link;
            if (r->strtab_old == 0 || r->strtab_old >= r->nin || f->sh[r->strtab_old].sh_type != SHT_STRTAB)
                return fail(-EINVAL, "the symbol table has no string table");
        }
    }
    return 0;
}

/* section_select decides which sections remain. */
static void section_select(struct rewrite *r)
{
    const struct elfrewrite_options *o = r->o;
    for (unsigned i = 1; i < r->nin; i++) {
        struct section *s = &r->sec[i];
        s->special = i == r->shstr_old || i == r->symtab_old || i == r->strtab_old;
        s->attached = !s->special && r->symtab_old && (s->sh.sh_type == SHT_REL || s->sh.sh_type == SHT_RELA) &&
                      s->sh.sh_link == r->symtab_old && s->sh.sh_info > 0 && s->sh.sh_info < r->nin &&
                      !(s->sh.sh_flags & SHF_ALLOC);
        s->retain = !s->special;
        if (!s->retain || s->attached)
            continue;
        if (list_match(&o->remove_sections, s->name))
            s->retain = 0;
        else if (o->only_sections.count && !list_match(&o->only_sections, s->name))
            s->retain = 0;
        else if (o->strip != ELFREWRITE_STRIP_NONE && is_debug_name(s->name))
            s->retain = 0;
    }
    for (unsigned i = 1; i < r->nin; i++) {
        struct section *s = &r->sec[i];
        if (s->attached)
            s->retain = !list_match(&o->remove_sections, s->name) && r->sec[s->sh.sh_info].retain;
    }
}

static void symbols_load(struct rewrite *r)
{
    if (!r->symtab_old)
        return;
    const struct section *st = &r->sec[r->symtab_old];
    const struct elffile *f = r->f;
    r->nsym = st->sh.sh_size / sizeof(Elf64_Sym);
    r->syms = bu_alloc((r->nsym + 1) * sizeof *r->syms);
    if (r->nsym)
        memcpy(r->syms, st->data, r->nsym * sizeof(Elf64_Sym));
    r->symname = bu_alloc((r->nsym + 1) * sizeof *r->symname);
    for (size_t i = 0; i < r->nsym; i++) {
        const char *name = elffile_string(f, &f->sh[r->strtab_old], r->syms[i].st_name);
        r->symname[i] = name ? name : "";
    }
}

/* symbols_select decides which symbols remain and numbers them: the local
 * symbols come first. */
static void symbols_select(struct rewrite *r)
{
    const struct elfrewrite_options *o = r->o;
    uint8_t *used = bu_alloc(r->nsym + 1);
    uint8_t *retain = bu_alloc(r->nsym + 1);
    memset(used, 0, r->nsym + 1);
    memset(retain, 0, r->nsym + 1);
    r->symmap = bu_alloc((r->nsym + 1) * sizeof *r->symmap);
    if (o->strip != ELFREWRITE_STRIP_ALL) {
        for (unsigned i = 1; i < r->nin; i++) {
            const struct section *s = &r->sec[i];
            if (!s->attached || !s->retain)
                continue;
            size_t entsize = s->sh.sh_entsize ? s->sh.sh_entsize : (s->sh.sh_type == SHT_RELA ? 24 : 16);
            for (size_t off = 0; off + entsize <= s->sh.sh_size; off += entsize) {
                uint64_t info;
                memcpy(&info, s->data + off + 8, sizeof info);
                if (ELF64_R_SYM(info) < r->nsym)
                    used[ELF64_R_SYM(info)] = 1;
            }
        }
    }
    for (size_t i = 0; i < r->nsym; i++) {
        const Elf64_Sym *s = &r->syms[i];
        r->symmap[i] = SYMBOL_DROPPED;
        if (i == 0) {
            retain[i] = 1;
            continue;
        }
        if (s->st_shndx != SHN_UNDEF && s->st_shndx < SHN_LORESERVE &&
            (s->st_shndx >= r->nin || !r->sec[s->st_shndx].retain))
            continue;
        int k = used[i] || symbol_retained(o, r->relocatable, s, r->symname[i]);
        if (k && !used[i] && list_match(&o->strip_symbols, r->symname[i]))
            k = 0;
        if (!k && list_match(&o->retain_symbols, r->symname[i]))
            k = 1;
        retain[i] = (uint8_t)k;
    }
    /* A symbol table that contains a symbol also contains the section
     * symbols of an executable or a shared object. */
    if (!r->relocatable && (o->strip == ELFREWRITE_STRIP_UNNEEDED || o->strip == ELFREWRITE_STRIP_ALL)) {
        size_t others = 0;
        for (size_t i = 1; i < r->nsym; i++)
            if (retain[i])
                others++;
        if (others > 0) {
            for (size_t i = 1; i < r->nsym; i++) {
                const Elf64_Sym *s = &r->syms[i];
                if (ELF64_ST_TYPE(s->st_info) == STT_SECTION && s->st_shndx < SHN_LORESERVE && s->st_shndx != 0 &&
                    s->st_shndx < r->nin && r->sec[s->st_shndx].retain)
                    retain[i] = 1;
            }
        }
    }
    /* An executable or a shared object lists its section symbols after
     * the other local symbols when a strip option applies. */
    int sections_last = !r->relocatable && o->strip != ELFREWRITE_STRIP_NONE;
    int has_section_symbol = 0;
    uint32_t next = 1;
    r->nlocal = 0;
    for (size_t i = 1; i < r->nsym; i++)
        if (ELF64_ST_TYPE(r->syms[i].st_info) == STT_SECTION)
            has_section_symbol = 1;
    for (int pass = 0; pass < (sections_last ? 2 : 1); pass++) {
        for (size_t i = 1; i < r->nsym; i++) {
            if (!retain[i] || !symbol_is_local(&r->syms[i]))
                continue;
            if (sections_last && (pass == 1) != (ELF64_ST_TYPE(r->syms[i].st_info) == STT_SECTION))
                continue;
            r->symmap[i] = next++;
            r->nlocal++;
        }
    }
    /* A new section of an executable receives a section symbol when the
     * symbol table has section symbols. */
    r->nadded_syms = 0;
    if (!r->relocatable && has_section_symbol && next > 1) {
        r->added_base = next;
        r->nadded_syms = o->added_count;
        next += (uint32_t)o->added_count;
        r->nlocal += o->added_count;
    }
    for (size_t i = 1; i < r->nsym; i++)
        if (retain[i] && !symbol_is_local(&r->syms[i]))
            r->symmap[i] = next++;
    r->nretained = next - 1;
    if (r->nsym)
        r->symmap[0] = 0;
    free(used);
    free(retain);
}

/* relocations_rewrite builds the relocation sections with the new symbol
 * indices.  A relocation section without relocations is removed. */
static int relocations_rewrite(struct rewrite *r)
{
    for (unsigned i = 1; i < r->nin; i++) {
        struct section *s = &r->sec[i];
        if (!s->attached || !s->retain)
            continue;
        size_t entsize = s->sh.sh_entsize ? s->sh.sh_entsize : (s->sh.sh_type == SHT_RELA ? 24 : 16);
        uint8_t *buf = bu_alloc(s->sh.sh_size + 1);
        size_t out = 0;
        for (size_t off = 0; off + entsize <= s->sh.sh_size; off += entsize) {
            uint64_t info;
            memcpy(&info, s->data + off + 8, sizeof info);
            uint64_t sym = ELF64_R_SYM(info);
            memcpy(buf + out, s->data + off, entsize);
            if (sym != 0) {
                if (sym >= r->nsym) {
                    free(buf);
                    return fail(-EINVAL, "relocation of '%s' uses an invalid symbol", s->name);
                }
                if (r->symmap[sym] == SYMBOL_DROPPED) {
                    if (r->o->strip == ELFREWRITE_STRIP_ALL)
                        continue;
                    free(buf);
                    return fail(-EINVAL, "symbol `%s' required but not present", r->symname[sym]);
                }
                info = ELF64_R_INFO(r->symmap[sym], ELF64_R_TYPE(info));
                memcpy(buf + out + 8, &info, sizeof info);
            }
            out += entsize;
        }
        if (out == 0) {
            s->retain = 0;
            free(buf);
            continue;
        }
        s->owned = buf;
        s->sh.sh_size = out;
    }
    return 0;
}

/* sections_number assigns the indices of the output. The added sections
 * and the rebuilt tables follow the copied sections. */
static unsigned sections_number(struct rewrite *r, unsigned *map, unsigned added_first)
{
    unsigned n = 1;
    for (unsigned i = 1; i < r->nin; i++)
        r->sec[i].index = r->sec[i].retain ? n++ : 0;
    for (size_t k = 0; k < r->o->added_count; k++)
        r->sec[added_first + k].index = n++;
    unsigned symtab = 0, strtab = 0;
    if (r->nretained > 0) {
        symtab = n++;
        strtab = n++;
    }
    unsigned shstr = n++;
    for (unsigned i = 0; i < r->nin; i++)
        map[i] = r->sec[i].retain ? r->sec[i].index : 0;
    if (r->symtab_old) {
        map[r->symtab_old] = symtab;
        map[r->strtab_old] = strtab;
    }
    map[r->shstr_old] = shstr;
    return n;
}

static int group_rewrite(struct rewrite *r, struct section *s, const unsigned *map)
{
    if (!s->data || s->sh.sh_size < 4)
        return 0;
    size_t words = s->sh.sh_size / 4;
    uint8_t *buf = bu_alloc(s->sh.sh_size);
    memcpy(buf, s->data, 4);
    size_t out = 1;
    for (size_t k = 1; k < words; k++) {
        uint32_t member;
        memcpy(&member, s->data + 4 * k, 4);
        if (member >= r->nin || !r->sec[member].retain)
            continue;
        member = map[member];
        memcpy(buf + 4 * out++, &member, 4);
    }
    s->owned = buf;
    s->sh.sh_size = out * 4;
    return 0;
}

static int links_fix(struct rewrite *r, const unsigned *map)
{
    for (unsigned i = 1; i < r->nin; i++) {
        struct section *s = &r->sec[i];
        if (!s->retain)
            continue;
        Elf64_Shdr *sh = &s->sh;
        sh->sh_link = sh->sh_link < r->nin ? map[sh->sh_link] : 0;
        if (sh->sh_type == SHT_GROUP) {
            if (sh->sh_info >= r->nsym || r->symmap[sh->sh_info] == SYMBOL_DROPPED)
                return fail(-EINVAL, "the signature symbol of group '%s' is removed", s->name);
            sh->sh_info = r->symmap[sh->sh_info];
            group_rewrite(r, s, map);
        } else if (sh->sh_type == SHT_REL || sh->sh_type == SHT_RELA || (sh->sh_flags & SHF_INFO_LINK)) {
            sh->sh_info = sh->sh_info < r->nin ? map[sh->sh_info] : 0;
        }
    }
    return 0;
}

/* The end of the contents that an executable or a shared object loads:
 * the headers, the segments and the allocated sections. */
static uint64_t loaded_end(const struct rewrite *r)
{
    const struct elffile *f = r->f;
    uint64_t end = sizeof(Elf64_Ehdr);
    if (f->nsegments && f->eh->e_phoff + (uint64_t)f->nsegments * f->eh->e_phentsize > end)
        end = f->eh->e_phoff + (uint64_t)f->nsegments * f->eh->e_phentsize;
    for (unsigned i = 0; i < f->nsegments; i++) {
        uint64_t seg_end = f->ph[i].p_offset + f->ph[i].p_filesz;
        if (seg_end > end)
            end = seg_end;
    }
    for (unsigned i = 1; i < r->nin; i++) {
        const Elf64_Shdr *sh = &r->sec[i].sh;
        if ((sh->sh_flags & SHF_ALLOC) && sh->sh_type != SHT_NOBITS && sh->sh_offset + sh->sh_size > end)
            end = sh->sh_offset + sh->sh_size;
    }
    if (end > f->size)
        end = f->size;
    return end;
}

/* section_place sets the file offset of s and copies its contents. */
static void section_place(struct rewrite *r, struct output *out, uint64_t *cursor, struct section *s,
                          const uint8_t *data)
{
    Elf64_Shdr *sh = &s->sh;
    if (!r->relocatable && (sh->sh_flags & SHF_ALLOC)) {
        if (data && sh->sh_type != SHT_NOBITS)
            output_put(out, sh->sh_offset, data, sh->sh_size);
        return;
    }
    *cursor = align_up(*cursor, sh->sh_addralign);
    sh->sh_offset = *cursor;
    if (sh->sh_type == SHT_NOBITS)
        return;
    output_put(out, *cursor, data, sh->sh_size);
    *cursor += sh->sh_size;
}

static void rewrite_free(struct rewrite *r)
{
    for (unsigned i = 0; i < r->nin; i++)
        free(r->sec[i].owned);
    free(r->sec);
    free(r->syms);
    free(r->symname);
    free(r->symmap);
}

int elfrewrite_image(const struct elffile *f, const struct elfrewrite_options *o, uint8_t **result, size_t *result_size)
{
    message[0] = '\0';
    *result = NULL;
    *result_size = 0;
    if (!f->sh || f->nsections == 0) {
        *result = bu_alloc(f->size ? f->size : 1);
        memcpy(*result, f->data, f->size);
        *result_size = f->size;
        return 0;
    }
    struct rewrite r;
    memset(&r, 0, sizeof r);
    r.f = f;
    r.o = o;
    r.nin = f->nsections;
    r.shstr_old = f->eh->e_shstrndx;
    r.relocatable = f->eh->e_type == ET_REL;
    if (r.shstr_old == 0 || r.shstr_old >= r.nin)
        return fail(-EINVAL, "the file has no section name table");
    /* The added sections follow the input sections in the working list. */
    r.sec = bu_alloc((r.nin + o->added_count) * sizeof *r.sec);
    memset(r.sec, 0, (r.nin + o->added_count) * sizeof *r.sec);
    int err = section_load(&r);
    if (err < 0) {
        rewrite_free(&r);
        return err;
    }
    section_select(&r);
    symbols_load(&r);
    if (r.symtab_old)
        symbols_select(&r);
    if ((err = relocations_rewrite(&r)) < 0) {
        rewrite_free(&r);
        return err;
    }
    for (size_t k = 0; k < o->added_count; k++) {
        struct section *s = &r.sec[r.nin + k];
        memset(s, 0, sizeof *s);
        s->name = o->added[k].section;
        s->data = o->added[k].data;
        s->sh.sh_type = SHT_PROGBITS;
        s->sh.sh_size = o->added[k].size;
        s->sh.sh_addralign = 1;
        s->retain = 1;
    }
    unsigned *map = bu_alloc(r.nin * sizeof *map);
    unsigned nout = sections_number(&r, map, r.nin);
    if (nout >= SHN_LORESERVE) {
        free(map);
        rewrite_free(&r);
        return fail(-ENOTSUP, "too many sections");
    }
    if ((err = links_fix(&r, map)) < 0) {
        free(map);
        rewrite_free(&r);
        return err;
    }
    unsigned symtab_idx = r.nretained > 0 ? nout - 3 : 0;
    unsigned strtab_idx = r.nretained > 0 ? nout - 2 : 0;
    unsigned shstr_idx = nout - 1;

    /* The symbol table and its string table. */
    struct strtab symstr, shstr;
    memset(&symstr, 0, sizeof symstr);
    memset(&shstr, 0, sizeof shstr);
    Elf64_Sym *outsyms = NULL;
    size_t *symname_id = NULL;
    if (r.nretained > 0) {
        outsyms = bu_alloc((r.nretained + 1) * sizeof *outsyms);
        symname_id = bu_alloc((r.nretained + 1) * sizeof *symname_id);
        memset(outsyms, 0, sizeof outsyms[0]);
        symname_id[0] = strtab_add(&symstr, "");
        for (size_t i = 1; i < r.nsym; i++) {
            uint32_t n = r.symmap[i];
            if (n == SYMBOL_DROPPED)
                continue;
            outsyms[n] = r.syms[i];
            if (outsyms[n].st_shndx != SHN_UNDEF && outsyms[n].st_shndx < SHN_LORESERVE)
                outsyms[n].st_shndx = (Elf64_Half)map[outsyms[n].st_shndx];
            symname_id[n] = strtab_add(&symstr, r.symname[i]);
        }
        for (size_t k = 0; k < r.nadded_syms; k++) {
            size_t n = r.added_base + k;
            memset(&outsyms[n], 0, sizeof outsyms[n]);
            outsyms[n].st_info = ELF64_ST_INFO(STB_LOCAL, STT_SECTION);
            outsyms[n].st_shndx = (Elf64_Half)r.sec[r.nin + k].index;
            symname_id[n] = strtab_add(&symstr, "");
        }
        strtab_finish(&symstr);
        for (size_t n = 0; n <= r.nretained; n++)
            outsyms[n].st_name = symstr.offset[symname_id[n]];
    }

    /* The section name table. */
    for (unsigned i = 1; i < r.nin; i++)
        if (r.sec[i].retain)
            r.sec[i].name_id = strtab_add(&shstr, r.sec[i].name);
    for (size_t k = 0; k < o->added_count; k++)
        r.sec[r.nin + k].name_id = strtab_add(&shstr, r.sec[r.nin + k].name);
    size_t symtab_name = 0, strtab_name = 0;
    if (r.nretained > 0) {
        symtab_name = strtab_add(&shstr, ".symtab");
        strtab_name = strtab_add(&shstr, ".strtab");
    }
    size_t shstr_name = strtab_add(&shstr, ".shstrtab");
    strtab_finish(&shstr);

    /* The layout. */
    struct output out;
    memset(&out, 0, sizeof out);
    uint64_t cursor;
    if (r.relocatable) {
        cursor = sizeof(Elf64_Ehdr);
    } else {
        cursor = loaded_end(&r);
        output_put(&out, 0, f->data, cursor);
    }
    Elf64_Shdr *shdrs = bu_alloc(nout * sizeof *shdrs);
    memset(shdrs, 0, nout * sizeof *shdrs);
    if (r.relocatable)
        output_put(&out, 0, f->data, sizeof(Elf64_Ehdr));
    for (unsigned i = 1; i < r.nin + o->added_count; i++) {
        struct section *s = &r.sec[i];
        if (!s->retain)
            continue;
        section_place(&r, &out, &cursor, s, s->owned ? s->owned : s->data);
        s->sh.sh_name = shstr.offset[s->name_id];
        shdrs[s->index] = s->sh;
    }
    if (r.nretained > 0) {
        Elf64_Shdr *sh = &shdrs[symtab_idx];
        sh->sh_name = shstr.offset[symtab_name];
        sh->sh_type = SHT_SYMTAB;
        sh->sh_size = (r.nretained + 1) * sizeof(Elf64_Sym);
        sh->sh_link = strtab_idx;
        sh->sh_info = (unsigned)(r.nlocal + 1);
        sh->sh_addralign = 8;
        sh->sh_entsize = sizeof(Elf64_Sym);
        cursor = align_up(cursor, 8);
        sh->sh_offset = cursor;
        output_put(&out, cursor, outsyms, sh->sh_size);
        cursor += sh->sh_size;
        sh = &shdrs[strtab_idx];
        sh->sh_name = shstr.offset[strtab_name];
        sh->sh_type = SHT_STRTAB;
        sh->sh_size = symstr.size;
        sh->sh_addralign = 1;
        sh->sh_offset = cursor;
        output_put(&out, cursor, symstr.data, symstr.size);
        cursor += symstr.size;
    }
    Elf64_Shdr *sh = &shdrs[shstr_idx];
    sh->sh_name = shstr.offset[shstr_name];
    sh->sh_type = SHT_STRTAB;
    sh->sh_size = shstr.size;
    sh->sh_addralign = 1;
    sh->sh_offset = cursor;
    output_put(&out, cursor, shstr.data, shstr.size);
    cursor += shstr.size;
    cursor = align_up(cursor, 8);
    output_put(&out, cursor, shdrs, nout * sizeof *shdrs);
    Elf64_Ehdr eh;
    memcpy(&eh, f->eh, sizeof eh);
    eh.e_shoff = cursor;
    eh.e_shentsize = sizeof(Elf64_Shdr);
    eh.e_shnum = (Elf64_Half)nout;
    eh.e_shstrndx = (Elf64_Half)shstr_idx;
    memcpy(out.data, &eh, sizeof eh);

    *result = out.data;
    *result_size = out.size;
    strtab_free(&symstr);
    strtab_free(&shstr);
    free(outsyms);
    free(symname_id);
    free(shdrs);
    free(map);
    rewrite_free(&r);
    return 0;
}

/* ---- raw binary output ---- */

/* binary_lma returns the load address of a section: the address plus the
 * distance of the physical address of its segment from the virtual
 * address. */
static uint64_t binary_lma(const struct elffile *f, const Elf64_Shdr *sh)
{
    if (f->eh->e_type == ET_REL)
        return sh->sh_addr;
    for (unsigned i = 0; i < f->nsegments; i++) {
        const Elf64_Phdr *p = &f->ph[i];
        if (p->p_type != PT_LOAD)
            continue;
        if (sh->sh_addr >= p->p_vaddr && sh->sh_addr + sh->sh_size <= p->p_vaddr + p->p_memsz &&
            sh->sh_offset >= p->p_offset && sh->sh_offset + sh->sh_size <= p->p_offset + p->p_filesz)
            return sh->sh_addr - p->p_vaddr + p->p_paddr;
    }
    return sh->sh_addr;
}

int elfrewrite_binary(const struct elffile *f, const struct elfrewrite_options *o, uint8_t **result, size_t *result_size)
{
    message[0] = '\0';
    *result = NULL;
    *result_size = 0;
    uint64_t low = 0, high = 0;
    int any = 0;
    for (unsigned i = 1; i < f->nsections; i++) {
        const Elf64_Shdr *sh = &f->sh[i];
        const char *name = elffile_section_name(f, sh);
        if (!(sh->sh_flags & SHF_ALLOC) || sh->sh_type == SHT_NOBITS || sh->sh_size == 0)
            continue;
        if (list_match(&o->remove_sections, name))
            continue;
        if (o->only_sections.count && !list_match(&o->only_sections, name))
            continue;
        if (!elffile_section_data(f, sh))
            return fail(-EINVAL, "section '%s' lies outside the file", name);
        uint64_t lma = binary_lma(f, sh);
        if (!any || lma < low)
            low = lma;
        if (!any || lma + sh->sh_size > high)
            high = lma + sh->sh_size;
        any = 1;
    }
    if (!any) {
        *result = bu_alloc(1);
        return 0;
    }
    if (high - low > ((uint64_t)1 << 32))
        return fail(-EFBIG, "the loadable contents are too large");
    uint8_t *buf = bu_alloc(high - low);
    memset(buf, 0, high - low);
    for (unsigned i = 1; i < f->nsections; i++) {
        const Elf64_Shdr *sh = &f->sh[i];
        const char *name = elffile_section_name(f, sh);
        if (!(sh->sh_flags & SHF_ALLOC) || sh->sh_type == SHT_NOBITS || sh->sh_size == 0)
            continue;
        if (list_match(&o->remove_sections, name))
            continue;
        if (o->only_sections.count && !list_match(&o->only_sections, name))
            continue;
        memcpy(buf + (binary_lma(f, sh) - low), elffile_section_data(f, sh), sh->sh_size);
    }
    *result = buf;
    *result_size = high - low;
    return 0;
}

/* ---- files and archives ---- */

static int format_check(const struct elfrewrite_options *o, const struct elffile *f, int binary)
{
    if (binary || !o->output_format)
        return 0;
    const char *name = o->output_format;
    if (strcmp(name, "elf64-little") == 0 || strcmp(name, "default") == 0)
        return 0;
    const char *own = elffile_bfd_name(f->eh->e_machine);
    if (own && strcmp(own, name) == 0)
        return 0;
    return -1;
}

static int convert(const struct elfrewrite_options *o, const struct elffile *f, int binary, uint8_t **out,
                   size_t *size)
{
    if (format_check(o, f, binary) < 0)
        return fail(-ENOEXEC, "cannot convert to the format '%s'", o->output_format);
    return binary ? elfrewrite_binary(f, o, out, size) : elfrewrite_image(f, o, out, size);
}

static int archive_convert(const char *in, const char *output, uint8_t *data, size_t size,
                           const struct elfrewrite_options *o)
{
    struct archive a;
    char err[200];
    if (archive_parse(&a, data, size, err, sizeof err) < 0) {
        bu_error(in, "%s", err);
        return 1;
    }
    int status = 0;
    for (struct ar_member *m = a.members; m; m = m->next) {
        /* A member starts at an even offset of the archive.  The copy is
         * aligned for the structures of the ELF file. */
        struct elffile mf;
        uint8_t *copy = bu_alloc(m->size ? m->size : 1);
        memcpy(copy, m->data, m->size);
        if (elffile_open(&mf, copy, m->size) < 0) {
            bu_error(in, "%s: file format not recognized", m->name);
            free(copy);
            status = 1;
            continue;
        }
        uint8_t *rewritten;
        size_t rewritten_size;
        int r = convert(o, &mf, 0, &rewritten, &rewritten_size);
        free(copy);
        if (r < 0) {
            bu_error(in, "%s: %s", m->name, message[0] ? message : strerror(-r));
            status = 1;
            continue;
        }
        free(m->owned);
        m->owned = rewritten;
        m->data = rewritten;
        m->size = rewritten_size;
    }
    if (!status) {
        int r = archive_write(&a, output, 1);
        if (r < 0) {
            bu_error(output, "%s", strerror(-r));
            status = 1;
        }
    }
    archive_free(&a);
    return status;
}

int elfrewrite_path(const char *in, const char *output, const struct elfrewrite_options *o, int binary)
{
    uint8_t *data;
    size_t size;
    int r = bu_read_file(in, &data, &size);
    if (r == -ENOENT) {
        bu_error(NULL, "'%s': No such file", in);
        return 1;
    }
    if (r < 0) {
        bu_error(in, "%s", strerror(-r));
        return 1;
    }
    struct stat st;
    if (stat(in, &st) < 0) {
        bu_error(in, "%s", strerror(errno));
        free(data);
        return 1;
    }
    unsigned mode = st.st_mode & 07777;
    const char *target = output ? output : in;
    int status = 0;
    if (archive_is(data, size)) {
        if (binary) {
            bu_error(in, "cannot write an archive as a binary file");
            status = 1;
        } else {
            status = archive_convert(in, target, data, size, o);
        }
    } else {
        struct elffile f;
        if (elffile_open(&f, data, size) < 0) {
            bu_error(in, "file format not recognized");
            status = 1;
        } else {
            uint8_t *result;
            size_t result_size;
            r = convert(o, &f, binary, &result, &result_size);
            if (r < 0) {
                bu_error(in, "%s", message[0] ? message : strerror(-r));
                status = 1;
            } else {
                r = bu_write_file(target, result, result_size, mode);
                if (r < 0) {
                    bu_error(target, "%s", strerror(-r));
                    status = 1;
                }
                free(result);
            }
        }
    }
    if (!status && o->preserve_dates) {
        struct utimbuf times;
        times.actime = st.st_atime;
        times.modtime = st.st_mtime;
        utime(target, &times);
    }
    free(data);
    return status;
}
