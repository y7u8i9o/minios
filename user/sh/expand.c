#include "sh.h"
#include <glob.h>
#include <fnmatch.h>

/* Per-byte flags distinguish quoted literals from splittable expansions.
 * A marker between quoted $@ arguments survives until field splitting. */
enum { QUOTED = 1, SPLIT = 2, BOUNDARY = 4 };
struct buffer {
    char *s;
    unsigned char *q;
    size_t n, cap;
    int retain;
};
static void append(struct buffer *b, const char *s, size_t n, int flags)
{
    if (b->n + n + 1 > b->cap) {
        b->cap = (b->n + n + 1) * 2;
        b->s = realloc(b->s, b->cap);
        b->q = realloc(b->q, b->cap);
        if (!b->s || !b->q) {
            fprintf(stderr, "sh: out of memory\n");
            exit(2);
        }
    }
    memcpy(b->s + b->n, s, n);
    memset(b->q + b->n, flags, n);
    b->n += n;
    b->s[b->n] = 0;
}
static void add_field(struct fields *f, const char *s, size_t n)
{
    f->v = realloc(f->v, (size_t)(f->n + 2) * sizeof *f->v);
    if (!f->v)
        exit(2);
    f->v[f->n++] = sh_slice(s, n);
    f->v[f->n] = NULL;
}
void fields_free(struct fields *f)
{
    for (int i = 0; i < f->n; i++)
        free(f->v[i]);
    free(f->v);
    f->v = NULL;
    f->n = 0;
}

static size_t closing(const char *s, size_t start, char open, char close)
{
    int depth = 1, quote = 0;
    size_t p = start;
    while (s[p]) {
        char c = s[p];
        if (c == '\\' && quote != '\'' && s[p + 1]) {
            p += 2;
            continue;
        }
        if (quote) {
            if (c == quote)
                quote = 0;
            p++;
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            p++;
            continue;
        }
        if (c == open)
            depth++;
        if (c == close && --depth == 0)
            return p;
        p++;
    }
    return p;
}
static void expand_text(struct buffer *b, const char *s, int force_quote, int heredoc);
static char *parameter(const char *expr)
{
    if (*expr == '#' && expr[1]) {
        char tmp[WORD_MAX];
        const char *v = var_lookup(expr + 1, tmp, sizeof tmp);
        size_t count = 0;
        for (; *v; v++)
            if (((unsigned char)*v & 0xc0) != 0x80)
                count++;
        char out[64];
        snprintf(out, sizeof out, "%lu", (unsigned long)count);
        return strdup(out);
    }
    const char *p = expr;
    if (strchr("?$#@*!", *p) && *p)
        p++;
    else
        while (isalnum((unsigned char)*p) || *p == '_')
            p++;
    char *name = sh_slice(expr, (size_t)(p - expr));
    char tmp[WORD_MAX];
    const char *v = var_lookup(name, tmp, sizeof tmp);
    int set = var_get(name) != NULL || !valid_name(name), null = !v || !*v;
    if (!*p) {
        char *out = strdup(v);
        free(name);
        return out;
    }
    int colon = *p == ':';
    if (colon)
        p++;
    char op = *p++;
    char *out = NULL;
    int missing = !set || (colon && null);
    if (strchr("-=+?", op)) {
        if ((op == '+' && !missing) || (op != '+' && missing)) {
            out = expand_one(p, 0);
            if (op == '=')
                var_set(name, out, 0);
            if (op == '?') {
                fprintf(stderr, "sh: %s: %s\n", name, *out ? out : "parameter unset");
                flow = FLOW_EXIT;
                last_status = 2;
            }
        } else
            out = strdup(op == '+' ? "" : v);
    } else if (op == '%' || op == '#') {
        int longest = *p == op;
        if (longest)
            p++;
        char *pat = expand_one(p, 1);
        size_t n = strlen(v);
        out = strdup(v);
        for (size_t k = 0; k <= n; k++) {
            size_t cut = longest ? n - k : k;
            char *part = op == '#' ? sh_slice(v, cut) : strdup(v + n - cut);
            int match = !fnmatch(pat, part, 0);
            free(part);
            if (match) {
                if (op == '#')
                    memmove(out, out + cut, n - cut + 1);
                else
                    out[n - cut] = 0;
                break;
            }
        }
        free(pat);
    } else {
        fprintf(stderr, "sh: bad substitution: %s\n", expr);
        out = strdup("");
        last_status = 2;
    }
    free(name);
    return out;
}
static void dollar(struct buffer *b, const char *s, size_t *position, int quoted)
{
    size_t p = *position + 1;
    char *value = NULL;
    if (s[p] == '(') {
        size_t end = closing(s, p + 1, '(', ')');
        int arithmetic = s[p + 1] == '(';
        char *body = sh_slice(s + p + 1 + (arithmetic ? 1 : 0), end - p - 1 - (arithmetic ? 2 : 0));
        if (arithmetic) {
            int error;
            long n = arith_eval(body, &error);
            char tmp[64];
            if (error) {
                fprintf(stderr, "sh: invalid arithmetic: %s\n", body);
                last_status = 2;
                flow = FLOW_EXIT;
            }
            snprintf(tmp, sizeof tmp, "%ld", n);
            value = strdup(tmp);
        } else
            value = capture(body);
        free(body);
        p = end + (s[end] != 0);
    } else if (s[p] == '{') {
        size_t end = closing(s, p + 1, '{', '}');
        char *expr = sh_slice(s + p + 1, end - p - 1);
        value = parameter(expr);
        free(expr);
        p = end + (s[end] != 0);
    } else {
        size_t start = p;
        if ((strchr("?$#@*!", s[p]) && s[p]) || isdigit((unsigned char)s[p]))
            p++;
        else
            while (isalnum((unsigned char)s[p]) || s[p] == '_')
                p++;
        if (p == start) {
            append(b, "$", 1, quoted ? QUOTED : 0);
            *position = p;
            return;
        }
        char *name = sh_slice(s + start, p - start);
        if (quoted && !strcmp(name, "@")) {
            for (int i = 1; i < script_argc; i++) {
                if (i > 1)
                    append(b, "\0", 1, BOUNDARY);
                append(b, script_argv[i], strlen(script_argv[i]), QUOTED);
                b->retain = 1;
            }
            free(name);
            *position = p;
            return;
        }
        char tmp[WORD_MAX];
        value = strdup(var_lookup(name, tmp, sizeof tmp));
        free(name);
    }
    append(b, value, strlen(value), quoted ? QUOTED : SPLIT);
    free(value);
    *position = p;
}
static void expand_text(struct buffer *b, const char *s, int force_quote, int heredoc)
{
    size_t p = 0;
    int quote = 0;
    if (!heredoc && s[0] == '~' && (s[1] == '/' || !s[1] || s[1] == ':')) {
        const char *home = var_get("HOME");
        if (home) {
            append(b, home, strlen(home), QUOTED);
            p = 1;
        }
    } else if (!heredoc && s[0] == '~') {
        /* ~name is the home of the account name. A word whose name part
         * contains anything but the characters of account names is left. */
        size_t n = strcspn(s + 1, "/:");
        char name[33];
        if (n < sizeof name && strspn(s + 1, "abcdefghijklmnopqrstuvwxyz0123456789_-") >= n) {
            memcpy(name, s + 1, n);
            name[n] = 0;
            struct passwd *pw = getpwnam(name);
            if (pw) {
                append(b, pw->pw_dir, strlen(pw->pw_dir), QUOTED);
                p = 1 + n;
            }
        }
    }
    while (s[p]) {
        char c = s[p];
        if (!heredoc && (c == '\'' || c == '"') && (!quote || quote == c)) {
            if (quote)
                quote = 0;
            else {
                quote = c;
                if (!(c == '"' && s[p + 1] == '$' && s[p + 2] == '@' && s[p + 3] == '"' && script_argc <= 1))
                    b->retain = 1;
            }
            p++;
            continue;
        }
        if (c == '\\' && quote != '\'' && s[p + 1] &&
            ((!quote && !heredoc) || strchr("$`\"\\\n", s[p + 1]))) {
            p++;
            if (s[p] != '\n')
                append(b, s + p, 1, QUOTED);
            p++;
            continue;
        }
        if (c == '$' && quote != '\'') {
            dollar(b, s, &p, quote || force_quote);
            continue;
        }
        append(b, s + p++, 1, quote || force_quote ? QUOTED : 0);
    }
}
char *expand_one(const char *text, int pattern)
{
    struct buffer b = {0};
    append(&b, "", 0, 0);
    expand_text(&b, text, 0, 0);
    if (pattern) {
        char *out = sh_alloc(b.n * 2 + 1);
        size_t n = 0;
        for (size_t i = 0; i < b.n; i++) {
            if ((b.q[i] & QUOTED) && strchr("*?[\\", b.s[i]))
                out[n++] = '\\';
            out[n++] = b.s[i];
        }
        free(b.s);
        b.s = out;
    }
    free(b.q);
    return b.s;
}
char *expand_here(const char *text)
{
    struct buffer b = {0};
    append(&b, "", 0, 0);
    expand_text(&b, text, 1, 1);
    free(b.q);
    return b.s;
}
static void glob_field(struct fields *f, struct buffer *b, size_t start, size_t end, int retain)
{
    if (start == end && !retain)
        return;
    char *pattern = sh_alloc((end - start) * 2 + 1);
    size_t n = 0;
    int magic = 0;
    for (size_t i = start; i < end; i++) {
        if (strchr("*?[\\", b->s[i])) {
            if (b->q[i] & QUOTED)
                pattern[n++] = '\\';
            else if (b->s[i] != '\\')
                magic = 1;
        }
        pattern[n++] = b->s[i];
    }
    glob_t g = {0};
    if (magic && !glob(pattern, 0, NULL, &g)) {
        for (size_t i = 0; i < g.gl_pathc; i++)
            add_field(f, g.gl_pathv[i], strlen(g.gl_pathv[i]));
    } else
        add_field(f, b->s + start, end - start);
    globfree(&g);
    free(pattern);
}
struct fields expand_words(struct word *w)
{
    struct fields f = {0};
    const char *ifs = var_get("IFS");
    if (!ifs)
        ifs = " \t\n";
    for (; w; w = w->next) {
        struct buffer b = {0};
        append(&b, "", 0, 0);
        expand_text(&b, w->text, 0, 0);
        size_t start = 0;
        int emitted = 0;
        for (size_t i = 0; i < b.n; i++) {
            int boundary = b.q[i] & BOUNDARY;
            if (boundary || ((b.q[i] & SPLIT) && strchr(ifs, b.s[i]))) {
                int white = !boundary && strchr(" \t\n", b.s[i]);
                if (i > start || !white) {
                    glob_field(&f, &b, start, i, !white);
                    emitted = 1;
                }
                start = i + 1;
                if (white) {
                    while (start < b.n && (b.q[start] & SPLIT) && strchr(ifs, b.s[start]) &&
                           strchr(" \t\n", b.s[start]))
                        start++;
                    i = start - 1;
                }
            }
        }
        int trailing_argument = b.n && (b.q[b.n - 1] & BOUNDARY);
        glob_field(&f, &b, start, b.n, trailing_argument || (b.retain && !emitted));
        free(b.s);
        free(b.q);
    }
    return f;
}
