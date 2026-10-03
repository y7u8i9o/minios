/* Manifest parsing, validation and writing; version comparison. */
#include "pkg.h"
#include <ctype.h>
#include <errno.h>
#include <fnmatch.h>
#include <stdlib.h>
#include <string.h>

int name_valid(const char *name)
{
    if (!*name || !(islower((unsigned char)*name) || isdigit((unsigned char)*name)))
        return 0;
    for (const char *p = name; *p; p++)
        if (!(islower((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '+' || *p == '.' || *p == '-'))
            return 0;
    return strlen(name) < PKG_NAME_MAX;
}

int version_valid(const char *v)
{
    if (!*v || strlen(v) >= PKG_VERSION_MAX)
        return 0;
    int digits = 0;
    for (const char *p = v; *p; p++) {
        if (isdigit((unsigned char)*p)) {
            digits++;
        } else if (*p == '.' && digits) {
            digits = 0;
        } else {
            return 0;
        }
    }
    return digits > 0;
}

/* Dotted integers compared component by component; a missing component
 * counts as zero. */
int version_cmp(const char *a, const char *b)
{
    while (*a || *b) {
        long x = *a ? strtol(a, (char **)&a, 10) : 0;
        long y = *b ? strtol(b, (char **)&b, 10) : 0;
        if (x != y)
            return x < y ? -1 : 1;
        if (*a == '.') a++;
        if (*b == '.') b++;
    }
    return 0;
}

int dep_satisfied(const struct pkg_dep *d, const char *version)
{
    if (!d->op[0])
        return 1;
    int c = version_cmp(version, d->version);
    if (strcmp(d->op, ">=") == 0) return c >= 0;
    if (strcmp(d->op, "=") == 0) return c == 0;
    if (strcmp(d->op, "<") == 0) return c < 0;
    return 0;
}

const struct pkg_lib *manifest_provides(const struct manifest *m, const char *soname)
{
    for (int i = 0; i < m->nprovides; i++)
        if (strcmp(m->provides[i].soname, soname) == 0)
            return &m->provides[i];
    return NULL;
}

int manifest_is_config(const struct manifest *m, const char *rel)
{
    for (int i = 0; i < m->nconfig; i++)
        if (strcmp(m->config[i], rel) == 0)
            return 1;
    return 0;
}

/* True if rel matches an unchecked pattern: a file of test data, such as
 * a deliberately broken ELF fixture, that the ELF and library rules leave
 * alone. A "*" also matches "/", as in the shell case statement of
 * tools/mkpkg.sh and in tools/mkbase.py. */
int manifest_is_unchecked(const struct manifest *m, const char *rel)
{
    for (int i = 0; i < m->nunchecked; i++)
        if (fnmatch(m->unchecked[i], rel, 0) == 0)
            return 1;
    return 0;
}

static int fail(char *err, size_t errlen, int line, const char *what, const char *value)
{
    if (value)
        snprintf(err, errlen, "manifest line %d: %s: %s", line, what, value);
    else
        snprintf(err, errlen, "manifest line %d: %s", line, what);
    return -1;
}

/* A path relative to the installation root: not absolute, no empty or dot
 * components. */
static int relative_valid(const char *p)
{
    if (!*p || *p == '/' || strlen(p) >= 128)
        return 0;
    for (const char *s = p; *s; ) {
        const char *e = strchr(s, '/');
        size_t n = e ? (size_t)(e - s) : strlen(s);
        if (n == 0 || (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.'))
            return 0;
        s += n;
        if (*s == '/') s++;
    }
    return 1;
}

static int parse_lib(struct pkg_lib *l, char *value)
{
    char *sp = strchr(value, ' ');
    if (!sp)
        return -1;
    *sp = '\0';
    char *end;
    long abi = strtol(sp + 1, &end, 10);
    if (*end || abi < 0 || abi > 100000 || strlen(value) >= PKG_NAME_MAX || strchr(value, '/'))
        return -1;
    strlcpy(l->soname, value, sizeof l->soname);
    l->abi = (int)abi;
    return 0;
}

/* A machine name: lowercase letters, digits and underscores. */
static int arch_valid(const char *value)
{
    size_t n = strlen(value);
    if (n == 0 || n >= 16)
        return 0;
    for (size_t i = 0; i < n; i++)
        if (!((value[i] >= 'a' && value[i] <= 'z') || (value[i] >= '0' && value[i] <= '9') || value[i] == '_'))
            return 0;
    return 1;
}

int manifest_parse(struct manifest *m, const char *text, size_t len, char *err, size_t errlen)
{
    memset(m, 0, sizeof *m);
    int line = 0;
    size_t at = 0;
    while (at < len) {
        size_t e = at;
        while (e < len && text[e] != '\n') e++;
        char buf[512];
        size_t n = e - at;
        if (n >= sizeof buf)
            return fail(err, errlen, line + 1, "line too long", NULL);
        memcpy(buf, text + at, n);
        buf[n] = '\0';
        at = e + 1;
        line++;
        char *p = buf;
        while (*p == ' ' || *p == '\t') p++;
        char *end = p + strlen(p);
        while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) *--end = '\0';
        if (!*p || *p == '#')
            continue;
        char *key = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (!*p)
            return fail(err, errlen, line, "key without value", key);
        *p++ = '\0';
        while (*p == ' ' || *p == '\t') p++;
        char *value = p;
        if (strcmp(key, "format") == 0) {
            if (m->format) return fail(err, errlen, line, "format given twice", NULL);
            char *fend;
            long f = strtol(value, &fend, 10);
            if (*fend || f < 1 || f > 1000) return fail(err, errlen, line, "invalid format", value);
            m->format = (int)f;
        } else if (strcmp(key, "name") == 0) {
            if (m->name[0]) return fail(err, errlen, line, "name given twice", NULL);
            if (!name_valid(value)) return fail(err, errlen, line, "invalid name", value);
            strlcpy(m->name, value, sizeof m->name);
        } else if (strcmp(key, "version") == 0) {
            if (m->version[0]) return fail(err, errlen, line, "version given twice", NULL);
            if (!version_valid(value)) return fail(err, errlen, line, "invalid version", value);
            strlcpy(m->version, value, sizeof m->version);
        } else if (strcmp(key, "summary") == 0) {
            if (m->summary[0]) return fail(err, errlen, line, "summary given twice", NULL);
            if (strlen(value) >= sizeof m->summary) return fail(err, errlen, line, "summary too long", NULL);
            strlcpy(m->summary, value, sizeof m->summary);
        } else if (strcmp(key, "arch") == 0) {
            if (m->arch[0]) return fail(err, errlen, line, "arch given twice", NULL);
            if (!arch_valid(value)) return fail(err, errlen, line, "invalid arch", value);
            strlcpy(m->arch, value, sizeof m->arch);
        } else if (strcmp(key, "depends") == 0) {
            if (m->ndeps >= PKG_MAX_DEPS) return fail(err, errlen, line, "too many depends lines", NULL);
            struct pkg_dep *d = &m->deps[m->ndeps];
            char *sp = strchr(value, ' ');
            if (sp) {
                *sp++ = '\0';
                while (*sp == ' ') sp++;
                char *sp2 = strchr(sp, ' ');
                if (!sp2) return fail(err, errlen, line, "depends needs an operator and a version", value);
                *sp2++ = '\0';
                while (*sp2 == ' ') sp2++;
                if (strcmp(sp, ">=") != 0 && strcmp(sp, "=") != 0 && strcmp(sp, "<") != 0)
                    return fail(err, errlen, line, "unknown operator", sp);
                if (!version_valid(sp2)) return fail(err, errlen, line, "invalid version", sp2);
                strlcpy(d->op, sp, sizeof d->op);
                strlcpy(d->version, sp2, sizeof d->version);
            }
            if (!name_valid(value)) return fail(err, errlen, line, "invalid package name", value);
            strlcpy(d->name, value, sizeof d->name);
            m->ndeps++;
        } else if (strcmp(key, "conflicts") == 0) {
            if (m->nconflicts >= PKG_MAX_DEPS) return fail(err, errlen, line, "too many conflicts lines", NULL);
            if (!name_valid(value)) return fail(err, errlen, line, "invalid package name", value);
            strlcpy(m->conflicts[m->nconflicts++], value, PKG_NAME_MAX);
        } else if (strcmp(key, "provides") == 0 || strcmp(key, "needs") == 0) {
            int provides = key[0] == 'p';
            int *count = provides ? &m->nprovides : &m->nneeds;
            struct pkg_lib *list = provides ? m->provides : m->needs;
            if (*count >= PKG_MAX_LIBS) return fail(err, errlen, line, "too many library lines", NULL);
            if (parse_lib(&list[*count], value) < 0) return fail(err, errlen, line, "expected a soname and an ABI number", value);
            (*count)++;
        } else if (strcmp(key, "launcher") == 0) {
            if (m->nlaunchers >= PKG_MAX_LAUNCHERS) return fail(err, errlen, line, "too many launcher lines", NULL);
            /* The final field is the command; the title may contain spaces. */
            char *sp = strrchr(value, ' ');
            if (!sp) return fail(err, errlen, line, "launcher needs a title and a command", value);
            *sp++ = '\0';
            while (*sp == ' ') sp++;
            if (strlen(value) >= 64 || strchr(value, '=')) return fail(err, errlen, line, "invalid launcher title", value);
            if (!relative_valid(sp)) return fail(err, errlen, line, "command must be a path relative to the root", sp);
            strlcpy(m->launchers[m->nlaunchers].title, value, 64);
            strlcpy(m->launchers[m->nlaunchers].command, sp, 128);
            m->nlaunchers++;
        } else if (strcmp(key, "mime-type") == 0) {
            if (m->ntypes >= PKG_MAX_MIME) return fail(err, errlen, line, "too many mime-type lines", NULL);
            char *sp = strchr(value, ' ');
            if (!sp || !strchr(value, '/')) return fail(err, errlen, line, "mime-type needs a type and extensions", value);
            *sp++ = '\0';
            while (*sp == ' ') sp++;
            if (strlen(value) >= 48 || strlen(sp) >= 64) return fail(err, errlen, line, "mime-type line too long", NULL);
            strlcpy(m->types[m->ntypes].type, value, 48);
            strlcpy(m->types[m->ntypes].extensions, sp, 64);
            m->ntypes++;
        } else if (strcmp(key, "mime-handler") == 0) {
            if (m->nhandlers >= PKG_MAX_MIME) return fail(err, errlen, line, "too many mime-handler lines", NULL);
            char *sp = strchr(value, ' ');
            if (!sp || !strchr(value, '/')) return fail(err, errlen, line, "mime-handler needs a type and a command", value);
            *sp++ = '\0';
            while (*sp == ' ') sp++;
            if (strlen(value) >= 48) return fail(err, errlen, line, "type too long", value);
            if (!relative_valid(sp)) return fail(err, errlen, line, "command must be a path relative to the root", sp);
            strlcpy(m->handlers[m->nhandlers].type, value, 48);
            strlcpy(m->handlers[m->nhandlers].command, sp, 128);
            m->nhandlers++;
        } else if (strcmp(key, "icon") == 0) {
            if (!relative_valid(value)) return fail(err, errlen, line, "icon must be a path relative to the root", value);
            strlcpy(m->icon, value, sizeof m->icon);
        } else if (strcmp(key, "unchecked") == 0) {
            if (m->nunchecked >= PKG_MAX_CONFIG) return fail(err, errlen, line, "too many unchecked lines", NULL);
            if (!relative_valid(value)) return fail(err, errlen, line, "unchecked must be a pattern relative to the root", value);
            strlcpy(m->unchecked[m->nunchecked++], value, sizeof m->unchecked[0]);
        } else if (strcmp(key, "config") == 0) {
            if (m->nconfig >= PKG_MAX_CONFIG) return fail(err, errlen, line, "too many config lines", NULL);
            if (!relative_valid(value)) return fail(err, errlen, line, "config must be a path relative to the root", value);
            if (manifest_is_config(m, value)) return fail(err, errlen, line, "config file named twice", value);
            strlcpy(m->config[m->nconfig++], value, sizeof m->config[0]);
        } else {
            return fail(err, errlen, line, "unknown key", key);
        }
    }
    if (!m->name[0]) { snprintf(err, errlen, "manifest without name"); return -1; }
    if (!m->version[0]) { snprintf(err, errlen, "manifest without version"); return -1; }
    if (!m->summary[0]) { snprintf(err, errlen, "manifest without summary"); return -1; }
    return 0;
}

int manifest_read(struct manifest *m, const char *path, char *err, size_t errlen)
{
    uint8_t *data;
    size_t len;
    if (read_file(path, &data, &len) < 0) {
        snprintf(err, errlen, "%s: %s", path, strerror(errno));
        return -1;
    }
    int r = manifest_parse(m, (const char *)data, len, err, errlen);
    free(data);
    return r;
}

void manifest_write(FILE *f, const struct manifest *m)
{
    if (m->format)
        fprintf(f, "format %d\n", m->format);
    fprintf(f, "name %s\nversion %s\nsummary %s\n", m->name, m->version, m->summary);
    if (m->arch[0])
        fprintf(f, "arch %s\n", m->arch);
    for (int i = 0; i < m->ndeps; i++) {
        if (m->deps[i].op[0])
            fprintf(f, "depends %s %s %s\n", m->deps[i].name, m->deps[i].op, m->deps[i].version);
        else
            fprintf(f, "depends %s\n", m->deps[i].name);
    }
    for (int i = 0; i < m->nconflicts; i++)
        fprintf(f, "conflicts %s\n", m->conflicts[i]);
    for (int i = 0; i < m->nprovides; i++)
        fprintf(f, "provides %s %d\n", m->provides[i].soname, m->provides[i].abi);
    for (int i = 0; i < m->nneeds; i++)
        fprintf(f, "needs %s %d\n", m->needs[i].soname, m->needs[i].abi);
    for (int i = 0; i < m->nlaunchers; i++)
        fprintf(f, "launcher %s %s\n", m->launchers[i].title, m->launchers[i].command);
    for (int i = 0; i < m->ntypes; i++)
        fprintf(f, "mime-type %s %s\n", m->types[i].type, m->types[i].extensions);
    for (int i = 0; i < m->nhandlers; i++)
        fprintf(f, "mime-handler %s %s\n", m->handlers[i].type, m->handlers[i].command);
    if (m->icon[0])
        fprintf(f, "icon %s\n", m->icon);
    for (int i = 0; i < m->nconfig; i++)
        fprintf(f, "config %s\n", m->config[i]);
    for (int i = 0; i < m->nunchecked; i++)
        fprintf(f, "unchecked %s\n", m->unchecked[i]);
}
