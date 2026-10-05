/* Archives in the System V format that the GNU binutils write: a member
 * header of 60 printable bytes, short names terminated with a slash, long
 * names in the "//" name table, member data padded to an even length.
 * The symbol index "/" comes first.  It contains a 32-bit big-endian
 * count, one 32-bit big-endian offset of a member header per symbol, and
 * the names of the symbols, each terminated with a zero byte.  BSD "#1/n"
 * names are understood when reading.  A 64-bit index "/SYM64/" is
 * skipped. */
#include "binutils.h"
#include <ar.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* A number field of the header: digits followed by spaces. */
static long field_number(const char *field, size_t len, int base)
{
    char buf[24];
    if (len >= sizeof buf)
        len = sizeof buf - 1;
    memcpy(buf, field, len);
    buf[len] = '\0';
    return strtol(buf, NULL, base);
}

static int fail(char *err, size_t errlen, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
    return -1;
}

int archive_is(const void *data, size_t size)
{
    return size >= SARMAG && memcmp(data, ARMAG, SARMAG) == 0;
}

void archive_append(struct archive *a, struct ar_member *m)
{
    m->next = NULL;
    struct ar_member **tail = &a->members;
    while (*tail)
        tail = &(*tail)->next;
    *tail = m;
}

int archive_parse(struct archive *a, const uint8_t *data, size_t size, char *err, size_t errlen)
{
    memset(a, 0, sizeof *a);
    if (!archive_is(data, size))
        return fail(err, errlen, "not an archive");
    const char *names = NULL;
    size_t names_len = 0;
    size_t off = SARMAG;
    while (off + sizeof(struct ar_hdr) <= size) {
        const struct ar_hdr *h = (const struct ar_hdr *)(data + off);
        if (memcmp(h->ar_fmag, ARFMAG, 2) != 0) {
            archive_free(a);
            return fail(err, errlen, "malformed archive");
        }
        size_t msize = (size_t)field_number(h->ar_size, sizeof h->ar_size, 10);
        const uint8_t *mdata = data + off + sizeof(struct ar_hdr);
        if (msize > size - off - sizeof(struct ar_hdr)) {
            archive_free(a);
            return fail(err, errlen, "malformed archive");
        }
        size_t next = off + sizeof(struct ar_hdr) + msize + (msize & 1);
        char *name;
        if (h->ar_name[0] == '/' && h->ar_name[1] == '/') {
            names = (const char *)mdata;
            names_len = msize;
            off = next;
            continue;
        }
        if (h->ar_name[0] == '/' && h->ar_name[1] == ' ') {
            a->index = mdata;
            a->index_size = msize;
            off = next;
            continue;
        }
        if (memcmp(h->ar_name, "/SYM64/", 7) == 0) {
            off = next;
            continue;
        }
        if (h->ar_name[0] == '/') {
            size_t pos = (size_t)field_number(h->ar_name + 1, sizeof h->ar_name - 1, 10);
            if (!names || pos >= names_len) {
                archive_free(a);
                return fail(err, errlen, "malformed archive");
            }
            const char *end = memchr(names + pos, '\n', names_len - pos);
            size_t len = end ? (size_t)(end - (names + pos)) : names_len - pos;
            if (len > 0 && names[pos + len - 1] == '/')
                len--;
            name = bu_strndup(names + pos, len);
        } else if (memcmp(h->ar_name, "#1/", 3) == 0) {
            size_t len = (size_t)field_number(h->ar_name + 3, sizeof h->ar_name - 3, 10);
            if (len > msize) {
                archive_free(a);
                return fail(err, errlen, "malformed archive");
            }
            name = bu_strndup((const char *)mdata, strnlen((const char *)mdata, len));
            mdata += len;
            msize -= len;
        } else {
            size_t len = 0;
            while (len < sizeof h->ar_name && h->ar_name[len] != '/' && h->ar_name[len] != ' ')
                len++;
            if (len == sizeof h->ar_name) {
                while (len > 0 && h->ar_name[len - 1] == ' ')
                    len--;
            }
            name = bu_strndup(h->ar_name, len);
        }
        struct ar_member *m = bu_alloc(sizeof *m);
        memset(m, 0, sizeof *m);
        /* A member starts at an even offset of the archive.  The ELF
         * structures need an alignment of 8 bytes, so each member is
         * copied into its own memory. */
        m->owned = bu_alloc(msize);
        memcpy(m->owned, mdata, msize);
        mdata = m->owned;
        m->name = name;
        m->date = field_number(h->ar_date, sizeof h->ar_date, 10);
        m->uid = (int)field_number(h->ar_uid, sizeof h->ar_uid, 10);
        m->gid = (int)field_number(h->ar_gid, sizeof h->ar_gid, 10);
        m->mode = (unsigned)field_number(h->ar_mode, sizeof h->ar_mode, 8);
        m->size = msize;
        m->data = mdata;
        m->offset = off;
        archive_append(a, m);
        off = next;
    }
    return 0;
}

void archive_free(struct archive *a)
{
    struct ar_member *m = a->members;
    while (m) {
        struct ar_member *next = m->next;
        free(m->name);
        free(m->owned);
        free(m);
        m = next;
    }
    a->members = NULL;
}

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

size_t archive_index_count(const struct archive *a)
{
    if (!a->index || a->index_size < 4)
        return 0;
    size_t n = be32(a->index);
    return n <= (a->index_size - 4) / 4 ? n : 0;
}

int archive_index_entry(const struct archive *a, size_t i, const char **name, size_t *offset)
{
    size_t n = archive_index_count(a);
    if (i >= n)
        return 0;
    *offset = be32(a->index + 4 + 4 * i);
    /* The names follow the offsets in the same order. */
    const char *p = (const char *)a->index + 4 + 4 * n, *end = (const char *)a->index + a->index_size;
    for (size_t k = 0; k < i && p < end; k++)
        p += strnlen(p, (size_t)(end - p)) + 1;
    if (p >= end)
        return 0;
    *name = p;
    return 1;
}

int archive_index_symbol(const Elf64_Sym *sym)
{
    unsigned bind = ELF64_ST_BIND(sym->st_info);
    return sym->st_shndx != SHN_UNDEF && (bind == STB_GLOBAL || bind == STB_WEAK || bind == STB_GNU_UNIQUE);
}

/* ---- writing ---- */

struct buffer {
    uint8_t *data;
    size_t len, cap;
};

static void put(struct buffer *b, const void *p, size_t n)
{
    if (b->len + n > b->cap) {
        b->cap = (b->len + n) * 2 + 4096;
        b->data = bu_realloc(b->data, b->cap);
    }
    memcpy(b->data + b->len, p, n);
    b->len += n;
}

static void put_field(char *field, size_t len, const char *text)
{
    size_t n = strlen(text);
    if (n > len)
        n = len;
    memcpy(field, text, n);
    memset(field + n, ' ', len - n);
}

static void put_number(char *field, size_t len, long value, int base)
{
    char buf[24];
    snprintf(buf, sizeof buf, base == 8 ? "%lo" : "%ld", value);
    put_field(field, len, buf);
}

static void put_header(struct buffer *b, const char *name, long date, int uid, int gid, unsigned mode, size_t size,
                       int special)
{
    struct ar_hdr h;
    put_field(h.ar_name, sizeof h.ar_name, name);
    if (special == 1) {
        /* The index has the owner 0 and the mode 0, as GNU ar writes it. */
        put_number(h.ar_date, sizeof h.ar_date, date, 10);
        put_field(h.ar_uid, sizeof h.ar_uid, "0");
        put_field(h.ar_gid, sizeof h.ar_gid, "0");
        put_field(h.ar_mode, sizeof h.ar_mode, "0");
    } else if (special == 2) {
        /* The name table has only a size, as GNU ar writes it. */
        put_field(h.ar_date, sizeof h.ar_date, "");
        put_field(h.ar_uid, sizeof h.ar_uid, "");
        put_field(h.ar_gid, sizeof h.ar_gid, "");
        put_field(h.ar_mode, sizeof h.ar_mode, "");
    } else {
        put_number(h.ar_date, sizeof h.ar_date, date, 10);
        put_number(h.ar_uid, sizeof h.ar_uid, uid, 10);
        put_number(h.ar_gid, sizeof h.ar_gid, gid, 10);
        put_number(h.ar_mode, sizeof h.ar_mode, (long)mode, 8);
    }
    put_number(h.ar_size, sizeof h.ar_size, (long)size, 10);
    memcpy(h.ar_fmag, ARFMAG, 2);
    put(b, &h, sizeof h);
}

/* index_names collects the index symbols of the members: for each symbol
 * the number of its member and its name. */
struct index_symbol {
    size_t member;
    const char *name;
};

static size_t collect_index(const struct archive *a, struct index_symbol **out, size_t *names_size)
{
    size_t n = 0, cap = 0, k = 0;
    *out = NULL;
    *names_size = 0;
    for (const struct ar_member *m = a->members; m; m = m->next, k++) {
        struct elffile f;
        if (elffile_open(&f, m->data, m->size) < 0 || f.eh->e_type != ET_REL)
            continue;
        const Elf64_Shdr *symtab = elffile_find_type(&f, SHT_SYMTAB);
        size_t count;
        const Elf64_Shdr *strtab;
        const Elf64_Sym *syms = elffile_symbols(&f, symtab, &count, &strtab);
        for (size_t i = 1; syms && i < count; i++) {
            if (!archive_index_symbol(&syms[i]))
                continue;
            const char *name = elffile_string(&f, strtab, syms[i].st_name);
            if (!name || !*name)
                continue;
            if (n == cap) {
                cap = cap ? cap * 2 : 64;
                *out = bu_realloc(*out, cap * sizeof **out);
            }
            (*out)[n].member = k;
            (*out)[n].name = name;
            *names_size += strlen(name) + 1;
            n++;
        }
    }
    return n;
}

int archive_write(const struct archive *a, const char *path, int index)
{
    struct buffer b = { NULL, 0, 0 };
    put(&b, ARMAG, SARMAG);

    /* The name table contains every name longer than 15 bytes.  GNU ar
     * pads the table to an even size with a newline and counts the newline
     * in the size of the member. */
    size_t table_len = 0;
    for (const struct ar_member *m = a->members; m; m = m->next)
        if (strlen(m->name) > 15)
            table_len += strlen(m->name) + 2;
    size_t table_pad = table_len & 1;
    table_len += table_pad;

    struct index_symbol *syms = NULL;
    size_t nsyms = 0, names_size = 0;
    if (index)
        nsyms = collect_index(a, &syms, &names_size);

    /* The offsets of the member headers follow from the sizes of the
     * index and of the name table. */
    /* GNU ar pads the index to an even size with a zero byte and counts
     * the byte in the size of the member. */
    size_t index_len = 4 + 4 * nsyms + names_size;
    index_len += index_len & 1;
    size_t pos = SARMAG;
    if (index && nsyms)
        pos += sizeof(struct ar_hdr) + index_len;
    if (table_len)
        pos += sizeof(struct ar_hdr) + table_len;
    size_t nmembers = 0;
    for (const struct ar_member *m = a->members; m; m = m->next)
        nmembers++;
    size_t *offsets = bu_alloc((nmembers + 1) * sizeof *offsets);
    size_t k = 0;
    for (const struct ar_member *m = a->members; m; m = m->next, k++) {
        offsets[k] = pos;
        pos += sizeof(struct ar_hdr) + m->size + (m->size & 1);
    }

    if (index && nsyms) {
        put_header(&b, "/", a->index_date, 0, 0, 0, index_len, 1);
        uint8_t word[4];
        put_be32(word, (uint32_t)nsyms);
        put(&b, word, 4);
        for (size_t i = 0; i < nsyms; i++) {
            put_be32(word, (uint32_t)offsets[syms[i].member]);
            put(&b, word, 4);
        }
        for (size_t i = 0; i < nsyms; i++)
            put(&b, syms[i].name, strlen(syms[i].name) + 1);
        if ((4 + 4 * nsyms + names_size) & 1)
            put(&b, "", 1);
    }
    if (table_len) {
        put_header(&b, "//", 0, 0, 0, 0, table_len, 2);
        for (const struct ar_member *m = a->members; m; m = m->next)
            if (strlen(m->name) > 15) {
                put(&b, m->name, strlen(m->name));
                put(&b, "/\n", 2);
            }
        if (table_pad)
            put(&b, "\n", 1);
    }
    size_t table_pos = 0;
    for (const struct ar_member *m = a->members; m; m = m->next) {
        char name[24];
        if (strlen(m->name) > 15) {
            snprintf(name, sizeof name, "/%zu", table_pos);
            table_pos += strlen(m->name) + 2;
        } else {
            snprintf(name, sizeof name, "%s/", m->name);
        }
        put_header(&b, name, m->date, m->uid, m->gid, m->mode, m->size, 0);
        put(&b, m->data, m->size);
        if (m->size & 1)
            put(&b, "\n", 1);
    }
    free(offsets);
    free(syms);
    struct stat st;
    unsigned mode = stat(path, &st) == 0 ? (unsigned)(st.st_mode & 07777) : 0644;
    int r = bu_write_file(path, b.data, b.len, mode);
    free(b.data);
    return r;
}
