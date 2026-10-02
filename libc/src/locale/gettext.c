/* Message catalogues (L3, docs/design/gettext.md).
 *
 * A catalogue is a GNU .mo file: a header, a table of the lengths and
 * offsets of the original strings, sorted by the strings, and a table of
 * the translations in the same order.  The translation of the empty string
 * is the header of the catalogue, whose Plural-Forms line gives the number
 * of plural forms and a C expression in n that selects one.
 *
 * The languages are the entries of LANGUAGE, separated by colons, or the
 * locale of LC_MESSAGES.  A locale of C or C.UTF-8 translates nothing, as
 * in GNU gettext.  For an entry ll_CC.UTF-8 the catalogues of ll_CC.UTF-8,
 * ll_CC and ll are tried in that order.  A catalogue is read once and
 * stays in memory.  The bindings, the current domain and the catalogues
 * are protected by lock. */
#include <fcntl.h>
#include <libintl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "locale_impl.h"
#include "../thread/tcb.h"

#define DEFAULT_DIR "/usr/share/locale"
#define MO_FILE_MAX (4 << 20)

/* A node of a Plural-Forms expression. */
struct node {
    char op;                    /* 'n', '#' number, '?' condition, '!' not, or a binary operator */
    unsigned long value;
    int a, b, c;                /* operand nodes */
};

struct catalog {
    char domain[64], dir[128], lang[40];
    unsigned char *data;        /* NULL when the file does not exist */
    size_t size;
    uint32_t count, originals, translations;
    int swap;
    int nplurals;
    struct node expr[64];
    int nnodes, root;
    struct catalog *next;
};

struct binding {
    char domain[64], dir[128];
    struct binding *next;
};

static struct __libc_lock lock = __LIBC_LOCK_INIT;
static char current_domain[64] = "messages";
static struct binding *bindings;
static struct catalog *catalogs;

/* The functions below parse and evaluate Plural-Forms expressions. */

struct parser {
    const char *s;
    struct catalog *c;
    int error;
};

static int new_node(struct parser *p, char op, unsigned long value, int a, int b, int c)
{
    if (p->c->nnodes >= (int)(sizeof p->c->expr / sizeof p->c->expr[0])) {
        p->error = 1;
        return 0;
    }
    struct node *n = &p->c->expr[p->c->nnodes];
    n->op = op;
    n->value = value;
    n->a = a;
    n->b = b;
    n->c = c;
    return p->c->nnodes++;
}

static void skip(struct parser *p)
{
    while (*p->s == ' ' || *p->s == '\t' || *p->s == '\n')
        p->s++;
}

/* match consumes the operator op, which has one or two characters. */
static int match(struct parser *p, const char *op)
{
    skip(p);
    size_t n = strlen(op);
    if (strncmp(p->s, op, n) != 0)
        return 0;
    /* "<" must not match the start of "<=", and "!" not the start of "!=". */
    if (n == 1 && (op[0] == '<' || op[0] == '>' || op[0] == '!' || op[0] == '=') && p->s[1] == '=')
        return 0;
    p->s += n;
    return 1;
}

static int parse_condition(struct parser *p);

static int parse_primary(struct parser *p)
{
    skip(p);
    if (match(p, "(")) {
        int e = parse_condition(p);
        if (!match(p, ")"))
            p->error = 1;
        return e;
    }
    if (match(p, "!"))
        return new_node(p, '!', 0, parse_primary(p), 0, 0);
    if (*p->s == 'n') {
        p->s++;
        return new_node(p, 'n', 0, 0, 0, 0);
    }
    if (*p->s >= '0' && *p->s <= '9') {
        unsigned long v = 0;
        while (*p->s >= '0' && *p->s <= '9')
            v = v * 10 + (unsigned long)(*p->s++ - '0');
        return new_node(p, '#', v, 0, 0, 0);
    }
    p->error = 1;
    return 0;
}

/* The binary levels from the lowest to the highest precedence.  Two
 * character operators are encoded with one character each. */
static const struct { const char *ops[4]; const char codes[4]; } levels[] = {
    { { "||" }, { '|' } },
    { { "&&" }, { '&' } },
    { { "==", "!=" }, { '=', 'N' } },
    { { "<=", ">=", "<", ">" }, { 'L', 'G', '<', '>' } },
    { { "+", "-" }, { '+', '-' } },
    { { "*", "/", "%" }, { '*', '/', '%' } },
};

static int parse_level(struct parser *p, int level)
{
    if (level == (int)(sizeof levels / sizeof levels[0]))
        return parse_primary(p);
    int left = parse_level(p, level + 1);
    for (;;) {
        int k = 0, found = -1;
        for (; k < 4 && levels[level].ops[k]; k++)
            if (match(p, levels[level].ops[k])) {
                found = k;
                break;
            }
        if (found < 0 || p->error)
            return left;
        int right = parse_level(p, level + 1);
        left = new_node(p, levels[level].codes[found], 0, left, right, 0);
    }
}

static int parse_condition(struct parser *p)
{
    int cond = parse_level(p, 0);
    if (match(p, "?")) {
        int yes = parse_condition(p);
        if (!match(p, ":"))
            p->error = 1;
        int no = parse_condition(p);
        return new_node(p, '?', 0, cond, yes, no);
    }
    return cond;
}

static unsigned long eval(const struct catalog *c, int i, unsigned long n)
{
    const struct node *e = &c->expr[i];
    unsigned long a = 0, b = 0;
    switch (e->op) {
    case 'n': return n;
    case '#': return e->value;
    case '!': return !eval(c, e->a, n);
    case '?': return eval(c, e->a, n) ? eval(c, e->b, n) : eval(c, e->c, n);
    }
    a = eval(c, e->a, n);
    if (e->op == '|')
        return a || eval(c, e->b, n);
    if (e->op == '&')
        return a && eval(c, e->b, n);
    b = eval(c, e->b, n);
    switch (e->op) {
    case '=': return a == b;
    case 'N': return a != b;
    case 'L': return a <= b;
    case 'G': return a >= b;
    case '<': return a < b;
    case '>': return a > b;
    case '+': return a + b;
    case '-': return a - b;
    case '*': return a * b;
    case '/': return b ? a / b : 0;
    case '%': return b ? a % b : 0;
    }
    return 0;
}

/* The functions below read catalogues. */

static uint32_t word(const struct catalog *c, size_t at)
{
    if (at + 4 > c->size)
        return 0;
    uint32_t v;
    memcpy(&v, c->data + at, 4);
    return c->swap ? __builtin_bswap32(v) : v;
}

/* string returns string i of a table, or NULL when it lies outside the
 * file. */
static const char *string(const struct catalog *c, uint32_t table, uint32_t i, uint32_t *len)
{
    uint32_t l = word(c, table + 8 * (size_t)i), off = word(c, table + 8 * (size_t)i + 4);
    if ((size_t)off + l >= c->size || c->data[off + l] != '\0')
        return NULL;
    if (len)
        *len = l;
    return (const char *)c->data + off;
}

static const char *find(const struct catalog *c, const char *msgid, uint32_t *len)
{
    uint32_t lo = 0, hi = c->count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const char *orig = string(c, c->originals, mid, NULL);
        if (!orig)
            return NULL;
        int r = strcmp(msgid, orig);
        if (r == 0)
            return string(c, c->translations, mid, len);
        if (r < 0)
            hi = mid;
        else
            lo = mid + 1;
    }
    return NULL;
}

/* read_plural_forms sets the plural rule from the header, or the rule of
 * English when the header has none. */
static void read_plural_forms(struct catalog *c)
{
    c->nplurals = 2;
    c->nnodes = 0;
    struct parser p = { "n != 1", c, 0 };
    const char *header = find(c, "", NULL);
    const char *line = header ? strstr(header, "Plural-Forms:") : NULL;
    if (line) {
        const char *np = strstr(line, "nplurals="), *pl = strstr(line, "plural=");
        if (np && pl) {
            c->nplurals = atoi(np + 9);
            p.s = pl + 7;
        }
    }
    c->root = parse_condition(&p);
    if (p.error || c->nplurals < 1) {
        c->nplurals = 2;
        c->nnodes = 0;
        p = (struct parser){ "n != 1", c, 0 };
        c->root = parse_condition(&p);
    }
}

static void load(struct catalog *c)
{
    char path[256];
    strlcpy(path, c->dir, sizeof path);
    strlcat(path, "/", sizeof path);
    strlcat(path, c->lang, sizeof path);
    strlcat(path, "/LC_MESSAGES/", sizeof path);
    strlcat(path, c->domain, sizeof path);
    strlcat(path, ".mo", sizeof path);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return;
    off_t size = lseek(fd, 0, SEEK_END);
    if (size < 28 || size > MO_FILE_MAX || lseek(fd, 0, SEEK_SET) != 0) {
        close(fd);
        return;
    }
    unsigned char *data = malloc((size_t)size);
    size_t n = 0;
    ssize_t r;
    while (data && n < (size_t)size && (r = read(fd, data + n, (size_t)size - n)) > 0)
        n += (size_t)r;
    close(fd);
    if (!data || n != (size_t)size) {
        free(data);
        return;
    }
    uint32_t magic;
    memcpy(&magic, data, 4);
    if (magic != 0x950412de && magic != 0xde120495) {
        free(data);
        return;
    }
    c->data = data;
    c->size = n;
    c->swap = magic == 0xde120495;
    c->count = word(c, 8);
    c->originals = word(c, 12);
    c->translations = word(c, 16);
    if ((size_t)c->originals + 8 * (size_t)c->count > n || (size_t)c->translations + 8 * (size_t)c->count > n) {
        free(data);
        c->data = NULL;
        return;
    }
    read_plural_forms(c);
}

static const char *binding_of(const char *domain)
{
    for (struct binding *b = bindings; b; b = b->next)
        if (strcmp(b->domain, domain) == 0)
            return b->dir;
    return DEFAULT_DIR;
}

/* catalog returns the catalogue of a domain and a language, read on first
 * use.  The caller has acquired lock. */
static struct catalog *catalog(const char *domain, const char *lang)
{
    const char *dir = binding_of(domain);
    for (struct catalog *c = catalogs; c; c = c->next)
        if (strcmp(c->domain, domain) == 0 && strcmp(c->lang, lang) == 0 && strcmp(c->dir, dir) == 0)
            return c;
    struct catalog *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    strlcpy(c->domain, domain, sizeof c->domain);
    strlcpy(c->dir, dir, sizeof c->dir);
    strlcpy(c->lang, lang, sizeof c->lang);
    load(c);
    c->next = catalogs;
    catalogs = c;
    return c;
}

/* translate looks msgid up in the catalogues of the languages and returns
 * its translation for n, or NULL. */
static const char *translate(const char *domain, const char *msgid, unsigned long n, int plural, int category)
{
    struct __locale_struct *loc = __locale_current();
    if (category < LC_COLLATE || category > LC_MESSAGES)
        category = LC_MESSAGES;
    const char *name = loc->cat[category]->name;
    if (strcmp(name, "C") == 0 || strcmp(name, "C.UTF-8") == 0)
        return NULL;
    const char *list = getenv("LANGUAGE");
    if (!list || !*list)
        list = name;
    const char *result = NULL;
    __libc_lock_lock(&lock);
    while (*list && !result) {
        char entry[40];
        size_t k = 0;
        while (*list && *list != ':' && k + 1 < sizeof entry)
            entry[k++] = *list++;
        entry[k] = '\0';
        while (*list == ':')
            list++;
        /* The entry itself, then without its codeset, then the language. */
        for (int form = 0; form < 3 && !result; form++) {
            char lang[40];
            strlcpy(lang, entry, sizeof lang);
            char *cut = form == 1 ? strchr(lang, '.') : form == 2 ? strpbrk(lang, "_.@") : NULL;
            if (form && !cut)
                continue;
            if (cut)
                *cut = '\0';
            if (!lang[0])
                continue;
            struct catalog *c = catalog(domain, lang);
            if (!c || !c->data)
                continue;
            uint32_t len;
            const char *t = find(c, msgid, &len);
            if (!t)
                continue;
            if (plural) {
                unsigned long index = eval(c, c->root, n);
                if (index >= (unsigned long)c->nplurals)
                    index = 0;
                const char *end = t + len;
                while (index-- > 0 && t < end)
                    t += strlen(t) + 1;
                if (t >= end)
                    continue;
            }
            if (*t)
                result = t;
        }
    }
    __libc_lock_unlock(&lock);
    return result;
}

/* The functions below are the interface of <libintl.h>. */

char *dcngettext(const char *domain, const char *msgid, const char *msgid_plural, unsigned long n, int category)
{
    if (!msgid)
        return NULL;
    char dom[64];
    __libc_lock_lock(&lock);
    strlcpy(dom, domain ? domain : current_domain, sizeof dom);
    __libc_lock_unlock(&lock);
    const char *t = translate(dom, msgid, n, msgid_plural != NULL, category);
    if (t)
        return (char *)t;
    return (char *)(msgid_plural && n != 1 ? msgid_plural : msgid);
}

char *dcgettext(const char *domain, const char *msgid, int category)
{
    return dcngettext(domain, msgid, NULL, 1, category);
}

char *dgettext(const char *domain, const char *msgid)
{
    return dcngettext(domain, msgid, NULL, 1, LC_MESSAGES);
}

char *gettext(const char *msgid)
{
    return dcngettext(NULL, msgid, NULL, 1, LC_MESSAGES);
}

char *dngettext(const char *domain, const char *msgid, const char *msgid_plural, unsigned long n)
{
    return dcngettext(domain, msgid, msgid_plural, n, LC_MESSAGES);
}

char *ngettext(const char *msgid, const char *msgid_plural, unsigned long n)
{
    return dcngettext(NULL, msgid, msgid_plural, n, LC_MESSAGES);
}

char *textdomain(const char *domain)
{
    static char result[64];
    __libc_lock_lock(&lock);
    if (domain)
        strlcpy(current_domain, *domain ? domain : "messages", sizeof current_domain);
    strlcpy(result, current_domain, sizeof result);
    __libc_lock_unlock(&lock);
    return result;
}

char *bindtextdomain(const char *domain, const char *dirname)
{
    if (!domain || !*domain)
        return NULL;
    __libc_lock_lock(&lock);
    struct binding *b = bindings;
    while (b && strcmp(b->domain, domain) != 0)
        b = b->next;
    char *result = NULL;
    if (dirname) {
        if (!b && (b = calloc(1, sizeof *b)) != NULL) {
            strlcpy(b->domain, domain, sizeof b->domain);
            b->next = bindings;
            bindings = b;
        }
        if (b)
            strlcpy(b->dir, dirname, sizeof b->dir);
    }
    result = b ? b->dir : (char *)DEFAULT_DIR;
    __libc_lock_unlock(&lock);
    return result;
}

char *bind_textdomain_codeset(const char *domain, const char *codeset)
{
    /* Every catalogue and every locale use UTF-8. */
    return (char *)"UTF-8";
}
