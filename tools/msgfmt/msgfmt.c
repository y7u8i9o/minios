/* msgfmt: compile a .po file into a GNU .mo file (L3,
 * docs/design/gettext.md).
 *
 *   msgfmt -o OUT.mo IN.po
 *
 * The tool reads msgctxt, msgid, msgid_plural, msgstr and msgstr[N]
 * entries with continued strings and the escapes \n \t \" \\.  Entries
 * marked fuzzy and entries without a translation are left out, except the
 * header.  The .mo file has the originals sorted by strcmp and no hash
 * table, which the C library does not use. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct entry {
    char *key, *value;
    size_t key_len, value_len;      /* without the final NUL */
};

static struct entry *entries;
static size_t count, cap;

struct buffer {
    char *data;
    size_t len, cap;
};

static void append(struct buffer *b, const char *s, size_t n)
{
    if (b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2;
        b->data = realloc(b->data, b->cap);
        if (!b->data) {
            perror("msgfmt");
            exit(1);
        }
    }
    memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = '\0';
}

/* unescape appends the quoted string that starts at s. */
static int unescape(const char *s, struct buffer *b)
{
    if (*s != '"')
        return -1;
    for (s++; *s && *s != '"'; s++) {
        char c = *s;
        if (c == '\\') {
            s++;
            c = *s == 'n' ? '\n' : *s == 't' ? '\t' : *s == 'r' ? '\r' : *s;
            if (!*s)
                return -1;
        }
        append(b, &c, 1);
    }
    return *s == '"' ? 0 : -1;
}

/* The entry being read. */
static struct buffer ctxt, id, plural, strs;
static int has_ctxt, has_plural, nstr, fuzzy;

/* flush adds the entry being read, unless it is fuzzy or untranslated, and
 * starts an empty one.  The header, whose msgid is empty, is added even
 * when it is marked fuzzy. */
static void flush(void)
{
    int empty = 1;
    for (size_t i = 0; i < strs.len; i++)
        empty &= strs.data[i] == '\0';
    if (nstr > 0 && !empty && !(fuzzy && id.len)) {
        if (count == cap) {
            cap = cap ? cap * 2 : 64;
            entries = realloc(entries, cap * sizeof *entries);
            if (!entries) {
                perror("msgfmt");
                exit(1);
            }
        }
        struct buffer key = { 0 };
        append(&key, "", 0);
        if (has_ctxt) {
            append(&key, ctxt.data ? ctxt.data : "", ctxt.len);
            append(&key, "\004", 1);
        }
        append(&key, id.data ? id.data : "", id.len);
        if (has_plural) {
            key.len++;              /* the NUL between msgid and msgid_plural */
            append(&key, plural.data ? plural.data : "", plural.len);
        }
        struct entry *e = &entries[count++];
        e->key = key.data;
        e->key_len = key.len;
        e->value = malloc(strs.len + 1);
        memcpy(e->value, strs.data, strs.len);
        e->value[strs.len] = '\0';
        e->value_len = strs.len;
    }
    ctxt.len = id.len = plural.len = strs.len = 0;
    has_ctxt = has_plural = nstr = fuzzy = 0;
}

static int compare(const void *a, const void *b)
{
    return strcmp(((const struct entry *)a)->key, ((const struct entry *)b)->key);
}

static void put32(FILE *f, uint32_t v)
{
    unsigned char b[4] = { (unsigned char)v, (unsigned char)(v >> 8), (unsigned char)(v >> 16), (unsigned char)(v >> 24) };
    fwrite(b, 1, 4, f);
}

int main(int argc, char **argv)
{
    if (argc != 4 || strcmp(argv[1], "-o") != 0) {
        fprintf(stderr, "usage: msgfmt -o OUT.mo IN.po\n");
        return 2;
    }
    FILE *in = fopen(argv[3], "r");
    if (!in) {
        perror(argv[3]);
        return 1;
    }
    struct buffer *target = NULL;
    int pending_fuzzy = 0, after_msgstr = 0, line_no = 0;
    char line[4096];
    while (fgets(line, sizeof line, in)) {
        line_no++;
        char *s = line;
        while (*s == ' ' || *s == '\t')
            s++;
        s[strcspn(s, "\r\n")] = '\0';
        if (*s == '#') {
            if (s[1] == ',' && strstr(s, "fuzzy"))
                pending_fuzzy = 1;
            continue;
        }
        if (!*s)
            continue;
        if (strncmp(s, "msgctxt ", 8) == 0 || (strncmp(s, "msgid ", 6) == 0 && (after_msgstr || !has_ctxt))) {
            if (after_msgstr)
                flush();
            after_msgstr = 0;
            fuzzy = pending_fuzzy;
            pending_fuzzy = 0;
        }
        if (strncmp(s, "msgctxt ", 8) == 0) {
            target = &ctxt;
            has_ctxt = 1;
        } else if (strncmp(s, "msgid_plural ", 13) == 0) {
            target = &plural;
            has_plural = 1;
        } else if (strncmp(s, "msgid ", 6) == 0) {
            target = &id;
        } else if (strncmp(s, "msgstr", 6) == 0) {
            if (nstr > 0) {
                append(&strs, "", 0);
                strs.len++;         /* the NUL between two plural forms */
            }
            nstr++;
            after_msgstr = 1;
            target = &strs;
        } else if (*s != '"') {
            fprintf(stderr, "%s:%d: unknown line\n", argv[3], line_no);
            return 1;
        }
        const char *q = strchr(s, '"');
        if (!q || !target || unescape(q, target) < 0) {
            fprintf(stderr, "%s:%d: malformed string\n", argv[3], line_no);
            return 1;
        }
    }
    flush();
    fclose(in);
    qsort(entries, count, sizeof *entries, compare);

    FILE *out = fopen(argv[2], "wb");
    if (!out) {
        perror(argv[2]);
        return 1;
    }
    uint32_t n = (uint32_t)count, originals = 28, translations = 28 + 8 * n, strings = 28 + 16 * n;
    put32(out, 0x950412de);
    put32(out, 0);
    put32(out, n);
    put32(out, originals);
    put32(out, translations);
    put32(out, 0);
    put32(out, strings);
    uint32_t at = strings;
    for (size_t i = 0; i < count; i++) {
        put32(out, (uint32_t)entries[i].key_len);
        put32(out, at);
        at += (uint32_t)entries[i].key_len + 1;
    }
    for (size_t i = 0; i < count; i++) {
        put32(out, (uint32_t)entries[i].value_len);
        put32(out, at);
        at += (uint32_t)entries[i].value_len + 1;
    }
    for (size_t i = 0; i < count; i++)
        fwrite(entries[i].key, 1, entries[i].key_len + 1, out);
    for (size_t i = 0; i < count; i++)
        fwrite(entries[i].value, 1, entries[i].value_len + 1, out);
    if (fclose(out) != 0) {
        perror(argv[2]);
        return 1;
    }
    return 0;
}
