#include "sh.h"

struct parser {
    struct lexer lx;
    int status, depth;
    struct redir *pending[128];
    int nhere;
};
static struct node *list(struct parser *p);
static struct node *command(struct parser *p);
static int token(struct parser *p)
{
    return p->lx.token.kind;
}
static int word_is(struct parser *p, const char *s)
{
    return token(p) == T_WORD && !strcmp(p->lx.token.text, s);
}
static void error(struct parser *p)
{
    p->status = token(p) == T_END ? 1 : 2;
}

static void advance(struct parser *p)
{
    if (token(p) == T_NL && p->nhere) {
        const char *s = p->lx.source;
        for (int i = 0; i < p->nhere; i++) {
            struct redir *r = p->pending[i];
            char *delimiter = word_literal(r->target->text);
            size_t len = strlen(delimiter), used = 0;
            char *body = sh_alloc(strlen(s + p->lx.pos) + 1);
            int found = 0;
            while (s[p->lx.pos]) {
                size_t start = p->lx.pos;
                while (s[p->lx.pos] && s[p->lx.pos] != '\n')
                    p->lx.pos++;
                size_t end = p->lx.pos;
                if (s[p->lx.pos])
                    p->lx.pos++;
                if (r->strip)
                    while (s[start] == '\t')
                        start++;
                if (end - start == len && !strncmp(s + start, delimiter, len)) {
                    found = 1;
                    break;
                }
                memcpy(body + used, s + start, end - start);
                used += end - start;
                body[used++] = '\n';
            }
            free(delimiter);
            r->body = body;
            if (!found)
                p->status = 1;
        }
        p->nhere = 0;
    }
    lex_next(&p->lx);
    if (p->lx.incomplete)
        p->status = 1;
}
static void newlines(struct parser *p)
{
    while (token(p) == T_NL && !p->status)
        advance(p);
}
static struct node *make(enum node_kind kind)
{
    struct node *n = sh_alloc(sizeof *n);
    n->kind = kind;
    return n;
}
static struct word *take_word(struct parser *p)
{
    struct word *w = sh_alloc(sizeof *w);
    w->text = strdup(p->lx.token.text);
    advance(p);
    return w;
}
static int expect(struct parser *p, const char *s)
{
    if (!word_is(p, s)) {
        error(p);
        return 0;
    }
    advance(p);
    return 1;
}
static int is_redir(int t)
{
    return t == T_IO || (t >= T_IN && t <= T_HERETAB);
}
static void redirection(struct parser *p, struct node *n)
{
    int fd = -1;
    if (token(p) == T_IO) {
        fd = atoi(p->lx.token.text);
        advance(p);
    }
    int t = token(p);
    if (t < T_IN || t > T_HERETAB || fd > 255) {
        error(p);
        return;
    }
    struct redir *r = sh_alloc(sizeof *r);
    r->fd = fd >= 0 ? fd : (t == T_IN || t == T_DUPIN || t >= T_HERE ? 0 : 1);
    r->kind = t == T_IN       ? R_IN
              : t == T_OUT    ? R_OUT
              : t == T_APPEND ? R_APPEND
              : t == T_DUPIN  ? R_DUPIN
              : t == T_DUPOUT ? R_DUPOUT
                              : R_HEREDOC;
    r->strip = t == T_HERETAB;
    struct redir **tail = &n->redirs;
    while (*tail)
        tail = &(*tail)->next;
    *tail = r;
    advance(p);
    if (token(p) != T_WORD) {
        error(p);
        return;
    }
    r->quoted = strpbrk(p->lx.token.text, "'\"\\") != NULL;
    r->target = take_word(p);
    if (r->kind == R_HEREDOC) {
        if (p->nhere == 128)
            p->status = 2;
        else
            p->pending[p->nhere++] = r;
    }
}
static struct node *conditional(struct parser *p)
{
    struct node *n = make(N_IF);
    advance(p);
    n->a = list(p);
    if (!expect(p, "then"))
        return n;
    n->b = list(p);
    if (word_is(p, "elif"))
        n->c = conditional(p);
    else {
        if (word_is(p, "else")) {
            advance(p);
            n->c = list(p);
        }
        expect(p, "fi");
    }
    return n;
}
static struct node *command(struct parser *p)
{
    if (++p->depth > 128) {
        p->status = 2;
        p->depth--;
        return NULL;
    }
    size_t start = p->lx.token.start;
    struct node *n = NULL;
    if (word_is(p, "if"))
        n = conditional(p);
    else if (word_is(p, "while") || word_is(p, "until")) {
        n = make(word_is(p, "while") ? N_WHILE : N_UNTIL);
        advance(p);
        n->a = list(p);
        if (expect(p, "do")) {
            n->b = list(p);
            expect(p, "done");
        }
    } else if (word_is(p, "for")) {
        n = make(N_FOR);
        advance(p);
        if (token(p) != T_WORD || !valid_name(p->lx.token.text))
            error(p);
        else {
            n->text = strdup(p->lx.token.text);
            advance(p);
            newlines(p);
            if (word_is(p, "in")) {
                n->op = 1;
                advance(p);
                struct word **tail = &n->words;
                while (token(p) == T_WORD) {
                    *tail = take_word(p);
                    tail = &(*tail)->next;
                }
            }
            if (token(p) == T_SEMI || token(p) == T_NL) {
                advance(p);
                newlines(p);
            }
            if (expect(p, "do")) {
                n->a = list(p);
                expect(p, "done");
            }
        }
    } else if (word_is(p, "case")) {
        n = make(N_CASE);
        advance(p);
        if (token(p) != T_WORD)
            error(p);
        else
            n->words = take_word(p);
        newlines(p);
        if (expect(p, "in")) {
            newlines(p);
            struct node **tail = &n->a;
            while (!word_is(p, "esac") && !p->status) {
                struct node *arm = make(N_GROUP);
                *tail = arm;
                tail = &arm->next;
                if (token(p) == T_LP)
                    advance(p);
                struct word **wt = &arm->words;
                for (;;) {
                    if (token(p) != T_WORD) {
                        error(p);
                        break;
                    }
                    *wt = take_word(p);
                    wt = &(*wt)->next;
                    if (token(p) != T_PIPE)
                        break;
                    advance(p);
                }
                if (token(p) != T_RP) {
                    error(p);
                    break;
                }
                advance(p);
                arm->a = list(p);
                if (token(p) == T_DSEMI) {
                    advance(p);
                    newlines(p);
                } else if (!word_is(p, "esac")) {
                    error(p);
                    break;
                }
            }
            expect(p, "esac");
        }
    } else if (token(p) == T_LP || word_is(p, "{")) {
        int sub = token(p) == T_LP;
        n = make(sub ? N_SUBSHELL : N_GROUP);
        advance(p);
        n->a = list(p);
        if (sub) {
            if (token(p) != T_RP)
                error(p);
            else
                advance(p);
        } else
            expect(p, "}");
    } else {
        n = make(N_SIMPLE);
        struct word **tail = &n->words;
        while (!p->status && (token(p) == T_WORD || is_redir(token(p)))) {
            if (is_redir(token(p)))
                redirection(p, n);
            else {
                *tail = take_word(p);
                tail = &(*tail)->next;
            }
            if (n->words && !n->words->next && !n->redirs && token(p) == T_LP) {
                if (!valid_name(n->words->text)) {
                    error(p);
                    break;
                }
                n->kind = N_FUNCDEF;
                advance(p);
                if (token(p) != T_RP) {
                    error(p);
                    break;
                }
                advance(p);
                newlines(p);
                n->a = command(p);
                break;
            }
        }
        if (!n->words && !n->redirs)
            error(p);
    }
    while (is_redir(token(p)) && !p->status)
        redirection(p, n);
    if (!n->text)
        n->text = sh_slice(p->lx.source + start, p->lx.token.start - start);
    p->depth--;
    return n;
}
static struct node *pipeline(struct parser *p)
{
    int negate = word_is(p, "!");
    if (negate)
        advance(p);
    struct node *n = command(p);
    if (token(p) == T_PIPE || negate) {
        struct node *pipe = make(N_PIPELINE);
        pipe->a = n;
        pipe->op = negate;
        struct node **tail = &n->next;
        while (token(p) == T_PIPE && !p->status) {
            advance(p);
            newlines(p);
            *tail = command(p);
            if (!*tail)
                break;
            tail = &(*tail)->next;
        }
        n = pipe;
    }
    return n;
}
static int stop(struct parser *p)
{
    return token(p) == T_END || token(p) == T_RP || token(p) == T_DSEMI || word_is(p, "then") ||
           word_is(p, "else") || word_is(p, "elif") || word_is(p, "fi") || word_is(p, "do") ||
           word_is(p, "done") || word_is(p, "esac") || word_is(p, "}");
}
static struct node *list(struct parser *p)
{
    struct node *head = NULL, **tail = &head;
    newlines(p);
    while (!p->status && !stop(p)) {
        struct node *n = pipeline(p);
        while (!p->status && (token(p) == T_AND || token(p) == T_OR)) {
            struct node *and = make(N_LIST);
            and->op = token(p);
            and->a = n;
            advance(p);
            newlines(p);
            and->b = pipeline(p);
            n = and;
        }
        if (!n)
            break;
        *tail = n;
        tail = &n->next;
        if (token(p) == T_AMP) {
            n->bg = 1;
            struct node *label = n->kind == N_PIPELINE ? n->a : n;
            if (label->text) {
                size_t length = strlen(label->text);
                char *text = sh_alloc(length + 2);
                memcpy(text, label->text, length);
                text[length] = '&';
                free(label->text);
                label->text = text;
            }
        }
        if (token(p) == T_SEMI || token(p) == T_AMP || token(p) == T_NL) {
            advance(p);
            newlines(p);
        } else if (!stop(p))
            error(p);
    }
    return head;
}
struct node *parse(const char *source, int *status)
{
    struct parser p = {.lx.source = source};
    lex_next(&p.lx);
    struct node *n = list(&p);
    if (token(&p) != T_END && !p.status)
        p.status = 2;
    if (p.lx.incomplete || p.nhere)
        p.status = 1;
    lex_destroy(&p.lx);
    *status = p.status;
    if (p.status) {
        node_free(n);
        return NULL;
    }
    return n;
}
static void words_free(struct word *w)
{
    while (w) {
        struct word *next = w->next;
        free(w->text);
        free(w->quote);
        free(w);
        w = next;
    }
}
void node_free(struct node *n)
{
    while (n) {
        struct node *next = n->next;
        node_free(n->a);
        node_free(n->b);
        node_free(n->c);
        words_free(n->words);
        struct redir *r = n->redirs;
        while (r) {
            struct redir *next_r = r->next;
            words_free(r->target);
            free(r->body);
            free(r);
            r = next_r;
        }
        free(n->text);
        free(n);
        n = next;
    }
}
static struct word *words_clone(const struct word *w)
{
    struct word *head = NULL, **tail = &head;
    for (; w; w = w->next) {
        *tail = sh_alloc(sizeof **tail);
        (*tail)->text = strdup(w->text);
        tail = &(*tail)->next;
    }
    return head;
}
struct node *node_clone(const struct node *n)
{
    if (!n)
        return NULL;
    struct node *out = make(n->kind);
    *out = *n;
    out->text = n->text ? strdup(n->text) : NULL;
    out->words = words_clone(n->words);
    out->a = node_clone(n->a);
    out->b = node_clone(n->b);
    out->c = node_clone(n->c);
    out->next = node_clone(n->next);
    out->redirs = NULL;
    struct redir **tail = &out->redirs;
    for (const struct redir *r = n->redirs; r; r = r->next) {
        *tail = sh_alloc(sizeof **tail);
        **tail = *r;
        (*tail)->target = words_clone(r->target);
        (*tail)->body = r->body ? strdup(r->body) : NULL;
        (*tail)->next = NULL;
        tail = &(*tail)->next;
    }
    return out;
}
