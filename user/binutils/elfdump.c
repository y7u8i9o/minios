/* The code that readelf and objdump share (elfdump.h). */
#include "elfdump.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- sections ---- */

int elfdump_section_matches(const struct elffile *f, unsigned index, const char *spec)
{
    char *end;
    unsigned long n = strtoul(spec, &end, 10);
    if (end != spec && *end == '\0')
        return n == index;
    const Elf64_Shdr *s = elffile_section(f, index);
    return s && strcmp(elffile_section_name(f, s), spec) == 0;
}

int elfdump_vma_to_offset(const struct elffile *f, uint64_t vma, uint64_t size, uint64_t *off)
{
    for (unsigned i = 0; i < f->nsegments; i++) {
        const Elf64_Phdr *p = &f->ph[i];
        if (p->p_type != PT_LOAD || vma < p->p_vaddr)
            continue;
        uint64_t rel = vma - p->p_vaddr;
        if (rel > p->p_filesz || size > p->p_filesz - rel)
            continue;
        *off = p->p_offset + rel;
        return 0;
    }
    return -ERANGE;
}

/* ---- relocations ---- */

long elfdump_read_relocs(const struct elffile *f, const Elf64_Shdr *sh, struct elfdump_reloc **out)
{
    *out = NULL;
    size_t entsize = sh->sh_type == SHT_RELA ? sizeof(Elf64_Rela) : sizeof(Elf64_Rel);
    const uint8_t *data = elffile_section_data(f, sh);
    if (!data)
        return -EINVAL;
    size_t n = sh->sh_size / entsize;
    struct elfdump_reloc *r = bu_alloc((n ? n : 1) * sizeof *r);
    for (size_t i = 0; i < n; i++) {
        uint64_t w[3] = { 0, 0, 0 };
        memcpy(w, data + i * entsize, entsize);
        r[i].offset = w[0];
        r[i].info = w[1];
        r[i].addend = sh->sh_type == SHT_RELA ? (int64_t)w[2] : 0;
    }
    *out = r;
    return (long)n;
}

/* ---- dynamic section ---- */

int elfdump_load_dynamic(const struct elffile *f, struct elfdump_dynamic *d)
{
    memset(d, 0, sizeof *d);
    uint64_t off = 0, size = 0;
    int have = 0;
    for (unsigned i = 0; i < f->nsegments && !have; i++) {
        const Elf64_Phdr *p = &f->ph[i];
        if (p->p_type == PT_DYNAMIC && p->p_offset <= f->size && p->p_filesz <= f->size - p->p_offset) {
            off = p->p_offset;
            size = p->p_filesz;
            have = 1;
        }
    }
    const Elf64_Shdr *sh = NULL;
    if (!have) {
        sh = elffile_find_type(f, SHT_DYNAMIC);
        if (!sh || !elffile_section_data(f, sh))
            return -ENOENT;
        off = sh->sh_offset;
        size = sh->sh_size;
    }
    d->entries = (const Elf64_Dyn *)(f->data + off);
    d->offset = off;
    size_t n = size / sizeof(Elf64_Dyn), i = 0;
    while (i < n && d->entries[i].d_tag != DT_NULL)
        i++;
    d->count = i < n ? i + 1 : n;

    int found = 0;
    uint64_t strtab = elfdump_dynamic_value(d, DT_STRTAB, &found);
    uint64_t strsz = elfdump_dynamic_value(d, DT_STRSZ, NULL);
    uint64_t soff;
    if (found && elfdump_vma_to_offset(f, strtab, strsz, &soff) == 0) {
        d->strtab = (const char *)f->data + soff;
        d->strsz = strsz;
        return 0;
    }
    if (!sh)
        sh = elffile_find_type(f, SHT_DYNAMIC);
    const Elf64_Shdr *str = sh ? elffile_linked(f, sh) : NULL;
    if (str && str->sh_type == SHT_STRTAB && elffile_section_data(f, str)) {
        d->strtab = (const char *)f->data + str->sh_offset;
        d->strsz = str->sh_size;
    }
    return 0;
}

uint64_t elfdump_dynamic_value(const struct elfdump_dynamic *d, int64_t tag, int *found)
{
    if (found)
        *found = 0;
    for (size_t i = 0; i < d->count; i++)
        if (d->entries[i].d_tag == tag) {
            if (found)
                *found = 1;
            return d->entries[i].d_un.d_val;
        }
    return 0;
}

const char *elfdump_dynamic_string(const struct elfdump_dynamic *d, uint64_t off)
{
    if (!d->strtab || off >= d->strsz || !memchr(d->strtab + off, '\0', d->strsz - off))
        return NULL;
    return d->strtab + off;
}

/* ---- symbol versions ---- */

#define VERSYM_HIDDEN 0x8000
#define VERSYM_VERSION 0x7fff

void elfdump_versions_init(const struct elffile *f, const Elf64_Shdr *symtab, struct elfdump_versions *v)
{
    memset(v, 0, sizeof *v);
    unsigned symtab_index = elffile_section_index(f, symtab);
    for (unsigned i = 0; i < f->nsections; i++) {
        const Elf64_Shdr *s = &f->sh[i];
        if (s->sh_type == SHT_GNU_versym && s->sh_link == symtab_index && !v->versym_sh) {
            v->versym_sh = s;
            v->versym = elffile_section_data(f, s);
            v->nversym = v->versym ? s->sh_size / 2 : 0;
        } else if (s->sh_type == SHT_GNU_verdef && !v->verdef_sh) {
            v->verdef_sh = s;
        } else if (s->sh_type == SHT_GNU_verneed && !v->verneed_sh) {
            v->verneed_sh = s;
        }
    }
}

/* in_bounds reports whether size bytes at off lie in the section data. */
static int in_bounds(const Elf64_Shdr *sh, uint64_t off, size_t size)
{
    return off <= sh->sh_size && size <= sh->sh_size - off;
}

const char *elfdump_version_name(const struct elffile *f, const struct elfdump_versions *v, unsigned index,
                                 int *is_need)
{
    if (is_need)
        *is_need = 0;
    index &= VERSYM_VERSION;
    if (v->verdef_sh) {
        const uint8_t *base = elffile_section_data(f, v->verdef_sh);
        const Elf64_Shdr *str = elffile_linked(f, v->verdef_sh);
        uint64_t off = 0;
        for (unsigned n = 0; base && n < 65536 && in_bounds(v->verdef_sh, off, sizeof(Elf64_Verdef)); n++) {
            Elf64_Verdef d;
            memcpy(&d, base + off, sizeof d);
            if (d.vd_ndx == index && d.vd_cnt > 0 && in_bounds(v->verdef_sh, off + d.vd_aux, sizeof(Elf64_Verdaux))) {
                Elf64_Verdaux a;
                memcpy(&a, base + off + d.vd_aux, sizeof a);
                return elffile_string(f, str, a.vda_name);
            }
            if (d.vd_next == 0)
                break;
            off += d.vd_next;
        }
    }
    if (v->verneed_sh) {
        const uint8_t *base = elffile_section_data(f, v->verneed_sh);
        const Elf64_Shdr *str = elffile_linked(f, v->verneed_sh);
        uint64_t off = 0;
        for (unsigned n = 0; base && n < 65536 && in_bounds(v->verneed_sh, off, sizeof(Elf64_Verneed)); n++) {
            Elf64_Verneed d;
            memcpy(&d, base + off, sizeof d);
            uint64_t aoff = off + d.vn_aux;
            for (unsigned k = 0; k < d.vn_cnt && in_bounds(v->verneed_sh, aoff, sizeof(Elf64_Vernaux)); k++) {
                Elf64_Vernaux a;
                memcpy(&a, base + aoff, sizeof a);
                if (a.vna_other == index) {
                    if (is_need)
                        *is_need = 1;
                    return elffile_string(f, str, a.vna_name);
                }
                if (a.vna_next == 0)
                    break;
                aoff += a.vna_next;
            }
            if (d.vn_next == 0)
                break;
            off += d.vn_next;
        }
    }
    return NULL;
}

const char *elfdump_format_symbol_name(const struct elffile *f, const struct elfdump_versions *v,
                                       const Elf64_Shdr *strtab, const Elf64_Sym *sym, size_t n, int need_index)
{
    static char buf[1024];
    const char *name = elffile_symbol_name(f, strtab, sym);
    if (!v->versym || n >= v->nversym)
        return name;
    unsigned data = v->versym[n];
    unsigned index = data & VERSYM_VERSION;
    if (index <= VER_NDX_GLOBAL && !(data & VERSYM_HIDDEN))
        return name;
    int is_need;
    const char *ver = elfdump_version_name(f, v, data, &is_need);
    if (!ver)
        return name;
    if (sym->st_shndx == SHN_ABS && strcmp(ver, name) == 0)
        return name;
    if (is_need) {
        if (need_index)
            snprintf(buf, sizeof buf, "%s@%s (%u)", name, ver, index);
        else
            snprintf(buf, sizeof buf, "%s@%s", name, ver);
    } else if (data & VERSYM_HIDDEN) {
        snprintf(buf, sizeof buf, "%s@%s", name, ver);
    } else {
        snprintf(buf, sizeof buf, "%s@@%s", name, ver);
    }
    return buf;
}
