/* mint: lexer and parser. See mint.c for the language summary. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mint.h"

/* ---- errors ---- */

static int cur_line = 1;

__attribute__((noreturn)) void fail(int line, const char *msg, const char *detail)
{
    fprintf(stderr, "mint: line %d: %s%s%s\n", line, msg, detail ? ": " : "", detail ? detail : "");
    exit(1);
}

/* ---- lexer ---- */

struct token {
    enum tok kind;
    long i;
    char *s;
    int line;
};

static const char *src;
static size_t pos;
static struct token tk;      /* current token */

static const struct { const char *w; enum tok k; } keywords[] = {
    { "if", T_IF }, { "elif", T_ELIF }, { "else", T_ELSE }, { "while", T_WHILE }, { "for", T_FOR },
    { "in", T_IN }, { "fn", T_FN }, { "return", T_RETURN }, { "break", T_BREAK }, { "continue", T_CONTINUE },
};

static void lex(void)
{
    free(tk.s);
    tk.s = NULL;
    for (;;) {
        while (src[pos] == ' ' || src[pos] == '\t' || src[pos] == '\r')
            pos++;
        if (src[pos] == '#') {
            while (src[pos] && src[pos] != '\n')
                pos++;
            continue;
        }
        break;
    }
    tk.line = cur_line;
    char c = src[pos];
    if (!c) { tk.kind = T_EOF; return; }
    if (c == '\n' || c == ';') {
        if (c == '\n')
            cur_line++;
        pos++;
        tk.kind = T_NL;
        return;
    }
    if (c >= '0' && c <= '9') {
        tk.i = strtol(src + pos, NULL, 10);
        while (src[pos] >= '0' && src[pos] <= '9')
            pos++;
        tk.kind = T_INT;
        return;
    }
    if (c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) {
        size_t start = pos;
        while (src[pos] == '_' || (src[pos] >= 'a' && src[pos] <= 'z') || (src[pos] >= 'A' && src[pos] <= 'Z') ||
               (src[pos] >= '0' && src[pos] <= '9'))
            pos++;
        tk.s = malloc(pos - start + 1);
        memcpy(tk.s, src + start, pos - start);
        tk.s[pos - start] = '\0';
        tk.kind = T_IDENT;
        for (size_t k = 0; k < sizeof keywords / sizeof keywords[0]; k++)
            if (strcmp(tk.s, keywords[k].w) == 0)
                tk.kind = keywords[k].k;
        return;
    }
    if (c == '"') {
        pos++;
        char *buf = malloc(strlen(src + pos) + 1);
        size_t n = 0;
        while (src[pos] && src[pos] != '"') {
            char ch = src[pos++];
            if (ch == '\\' && src[pos]) {
                char e = src[pos++];
                ch = e == 'n' ? '\n' : e == 't' ? '\t' : e;
            }
            buf[n++] = ch;
        }
        if (src[pos] != '"')
            fail(tk.line, "unterminated string", NULL);
        pos++;
        buf[n] = '\0';
        tk.s = buf;
        tk.kind = T_STR;
        return;
    }
    pos++;
    char d = src[pos];
    switch (c) {
    case '(': tk.kind = T_LP; return;
    case ')': tk.kind = T_RP; return;
    case '{': tk.kind = T_LB; return;
    case '}': tk.kind = T_RB; return;
    case ',': tk.kind = T_COMMA; return;
    case '+': tk.kind = T_PLUS; return;
    case '-': tk.kind = T_MINUS; return;
    case '*': tk.kind = T_STAR; return;
    case '/': tk.kind = T_SLASH; return;
    case '%': tk.kind = T_PERCENT; return;
    case '=': if (d == '=') { pos++; tk.kind = T_EQEQ; } else tk.kind = T_EQ; return;
    case '!': if (d == '=') { pos++; tk.kind = T_NE; } else tk.kind = T_NOT; return;
    case '<': if (d == '=') { pos++; tk.kind = T_LE; } else tk.kind = T_LT; return;
    case '>': if (d == '=') { pos++; tk.kind = T_GE; } else tk.kind = T_GT; return;
    case '&': if (d == '&') { pos++; tk.kind = T_AND; return; } break;
    case '|': if (d == '|') { pos++; tk.kind = T_OR; return; } break;
    }
    char msg[2] = { c, 0 };
    fail(tk.line, "unexpected character", msg);
}

/* ---- syntax tree ---- */

static struct node *node_new(enum nkind kind, int line)
{
    struct node *n = calloc(1, sizeof *n);
    n->kind = kind;
    n->line = line;
    return n;
}

static void node_add(struct node *n, struct node *kid)
{
    n->kids = realloc(n->kids, (size_t)(n->nkids + 1) * sizeof *n->kids);
    n->kids[n->nkids++] = kid;
}

static void expect(enum tok k, const char *what)
{
    if (tk.kind != k)
        fail(tk.line, "expected", what);
    lex();
}

static void skip_newlines(void)
{
    while (tk.kind == T_NL)
        lex();
}

static struct node *parse_expr(void);
static struct node *parse_block(void);

static struct node *parse_primary(void)
{
    struct node *n;
    switch (tk.kind) {
    case T_INT:
        n = node_new(N_INT, tk.line);
        n->i = tk.i;
        lex();
        return n;
    case T_STR:
        n = node_new(N_STR, tk.line);
        n->s = strdup(tk.s);
        lex();
        return n;
    case T_IDENT: {
        char *name = strdup(tk.s);
        int line = tk.line;
        lex();
        if (tk.kind == T_LP) {
            lex();
            n = node_new(N_CALL, line);
            n->s = name;
            while (tk.kind != T_RP) {
                node_add(n, parse_expr());
                if (tk.kind == T_COMMA)
                    lex();
                else if (tk.kind != T_RP)
                    fail(tk.line, "expected", ", or )");
            }
            lex();
            return n;
        }
        n = node_new(N_VAR, line);
        n->s = name;
        return n;
    }
    case T_LP:
        lex();
        n = parse_expr();
        expect(T_RP, ")");
        return n;
    case T_MINUS:
    case T_NOT: {
        n = node_new(N_UNOP, tk.line);
        n->op = tk.kind;
        lex();
        node_add(n, parse_primary());
        return n;
    }
    default:
        fail(tk.line, "expected an expression", NULL);
    }
}

static int precedence(enum tok k)
{
    switch (k) {
    case T_OR: return 1;
    case T_AND: return 2;
    case T_EQEQ: case T_NE: return 3;
    case T_LT: case T_LE: case T_GT: case T_GE: return 4;
    case T_PLUS: case T_MINUS: return 5;
    case T_STAR: case T_SLASH: case T_PERCENT: return 6;
    default: return 0;
    }
}

static struct node *parse_binary(int min_prec)
{
    struct node *left = parse_primary();
    for (;;) {
        int p = precedence(tk.kind);
        if (!p || p < min_prec)
            return left;
        struct node *n = node_new(N_BINOP, tk.line);
        n->op = tk.kind;
        lex();
        node_add(n, left);
        node_add(n, parse_binary(p + 1));
        left = n;
    }
}

static struct node *parse_expr(void)
{
    return parse_binary(1);
}

static struct node *parse_statement(void)
{
    struct node *n;
    int line = tk.line;
    switch (tk.kind) {
    case T_IF: {
        n = node_new(N_IF, line);
        lex();
        node_add(n, parse_expr());          /* cond, block, [cond, block]..., [else block] */
        node_add(n, parse_block());
        while (tk.kind == T_ELIF) {
            lex();
            node_add(n, parse_expr());
            node_add(n, parse_block());
        }
        if (tk.kind == T_ELSE) {
            lex();
            node_add(n, parse_block());
        }
        return n;
    }
    case T_WHILE:
        n = node_new(N_WHILE, line);
        lex();
        node_add(n, parse_expr());
        node_add(n, parse_block());
        return n;
    case T_FOR: {
        n = node_new(N_FOR, line);
        lex();
        if (tk.kind != T_IDENT)
            fail(tk.line, "expected a loop variable", NULL);
        n->s = strdup(tk.s);
        lex();
        expect(T_IN, "in");
        if (tk.kind != T_IDENT || strcmp(tk.s, "range") != 0)
            fail(tk.line, "expected range(", NULL);
        lex();
        expect(T_LP, "(");
        node_add(n, parse_expr());
        expect(T_COMMA, ",");
        node_add(n, parse_expr());
        expect(T_RP, ")");
        node_add(n, parse_block());
        return n;
    }
    case T_FN: {
        n = node_new(N_FN, line);
        lex();
        if (tk.kind != T_IDENT)
            fail(tk.line, "expected a function name", NULL);
        n->s = strdup(tk.s);
        lex();
        expect(T_LP, "(");
        struct node *params = node_new(N_BLOCK, line);
        while (tk.kind == T_IDENT) {
            struct node *p = node_new(N_VAR, tk.line);
            p->s = strdup(tk.s);
            node_add(params, p);
            lex();
            if (tk.kind == T_COMMA)
                lex();
        }
        expect(T_RP, ")");
        node_add(n, params);
        node_add(n, parse_block());
        return n;
    }
    case T_RETURN:
        n = node_new(N_RETURN, line);
        lex();
        if (tk.kind != T_NL && tk.kind != T_RB && tk.kind != T_EOF)
            node_add(n, parse_expr());
        return n;
    case T_BREAK:
        lex();
        return node_new(N_BREAK, line);
    case T_CONTINUE:
        lex();
        return node_new(N_CONTINUE, line);
    default:
        break;
    }
    struct node *e = parse_expr();
    if (tk.kind == T_EQ) {
        if (e->kind != N_VAR)
            fail(line, "cannot assign to this expression", NULL);
        lex();
        n = node_new(N_ASSIGN, line);
        n->s = e->s;
        node_add(n, parse_expr());
        return n;
    }
    n = node_new(N_EXPR, line);
    node_add(n, e);
    return n;
}

static struct node *parse_block(void)
{
    struct node *b = node_new(N_BLOCK, tk.line);
    expect(T_LB, "{");
    skip_newlines();
    while (tk.kind != T_RB) {
        if (tk.kind == T_EOF)
            fail(tk.line, "unexpected end of file, missing }", NULL);
        node_add(b, parse_statement());
        if (tk.kind != T_NL && tk.kind != T_RB)
            fail(tk.line, "expected end of statement", NULL);
        skip_newlines();
    }
    lex();
    return b;
}

struct node *mint_parse(const char *text)
{
    src = text;
    lex();
    struct node *b = node_new(N_BLOCK, 1);
    skip_newlines();
    while (tk.kind != T_EOF) {
        node_add(b, parse_statement());
        if (tk.kind != T_NL && tk.kind != T_EOF)
            fail(tk.line, "expected end of statement", NULL);
        skip_newlines();
    }
    return b;
}

