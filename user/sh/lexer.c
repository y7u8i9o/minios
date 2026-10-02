#include "sh.h"

void *sh_alloc(size_t n)
{
    void *p = calloc(1, n);
    if (!p) {
        fprintf(stderr, "sh: out of memory\n");
        exit(2);
    }
    return p;
}

char *sh_slice(const char *s, size_t n)
{
    char *p = sh_alloc(n + 1);
    memcpy(p, s, n);
    return p;
}

static size_t substitution(struct lexer *lx, size_t p)
{
    const char *s = lx->source;
    char closing = s[p] == '(' ? ')' : '}';
    char opening = s[p++];
    int depth = 1, quote = 0;
    while (s[p]) {
        char c = s[p++];
        if (c == '\\' && quote != '\'' && s[p]) {
            p++;
            continue;
        }
        if (quote) {
            if (c == quote)
                quote = 0;
            continue;
        }
        if (c == '\'' || c == '"') {
            quote = c;
            continue;
        }
        if (c == opening)
            depth++;
        if (c == closing && --depth == 0)
            return p;
    }
    lx->incomplete = 1;
    return p;
}

void lex_next(struct lexer *lx)
{
    free(lx->token.text);
    lx->token.text = NULL;
    const char *s = lx->source;
    size_t p = lx->pos;
    for (;;) {
        while (s[p] == ' ' || s[p] == '\t' || s[p] == '\r')
            p++;
        if (s[p] == '\\' && s[p + 1] == '\n') {
            p += 2;
            if (!s[p])
                lx->incomplete = 1;     /* the line continues on the next one */
            continue;
        }
        if (s[p] == '#')
            while (s[p] && s[p] != '\n')
                p++;
        break;
    }
    lx->token.start = p;
    enum token_kind t = T_WORD;
    size_t step = 1;
    switch (s[p]) {
    case 0:
        t = T_END;
        step = 0;
        break;
    case '\n':
        t = T_NL;
        break;
    case '|':
        t = s[p + 1] == '|' ? (step = 2, T_OR) : T_PIPE;
        break;
    case '&':
        t = s[p + 1] == '&' ? (step = 2, T_AND) : T_AMP;
        break;
    case ';':
        t = s[p + 1] == ';' ? (step = 2, T_DSEMI) : T_SEMI;
        break;
    case '(':
        t = T_LP;
        break;
    case ')':
        t = T_RP;
        break;
    case '<':
        t = T_IN;
        if (s[p + 1] == '&') {
            step = 2;
            t = T_DUPIN;
        }
        if (s[p + 1] == '<') {
            step = 2;
            t = T_HERE;
            if (s[p + 2] == '-') {
                step++;
                t = T_HERETAB;
            }
        }
        break;
    case '>':
        t = T_OUT;
        if (s[p + 1] == '&') {
            step = 2;
            t = T_DUPOUT;
        }
        if (s[p + 1] == '>') {
            step = 2;
            t = T_APPEND;
        }
        break;
    }
    if (t != T_WORD)
        p += step;
    else {
        size_t number = p;
        while (isdigit((unsigned char)s[number]))
            number++;
        if (number > p && (s[number] == '<' || s[number] == '>')) {
            p = number;
            t = T_IO;
        } else {
            int quote = 0;
            while (s[p]) {
                char c = s[p];
                if (!quote && strchr(" \t\r\n|&;()<>", c))
                    break;
                if (c == '\\' && quote != '\'') {
                    if (!s[p + 1]) {
                        lx->incomplete = 1;
                        p++;
                        break;
                    }
                    if (s[p + 1] == '\n' && !s[p + 2])
                        lx->incomplete = 1;     /* a word continued on the next line */
                    p += 2;
                    continue;
                }
                if (c == '$' && quote != '\'' && (s[p + 1] == '(' || s[p + 1] == '{')) {
                    p = substitution(lx, p + 1);
                    continue;
                }
                if (c == quote)
                    quote = 0;
                else if (!quote && (c == '\'' || c == '"'))
                    quote = c;
                p++;
            }
            if (quote)
                lx->incomplete = 1;
        }
        lx->token.text = sh_slice(s + lx->token.start, p - lx->token.start);
    }
    lx->token.kind = t;
    lx->pos = lx->token.end = p;
}

void lex_destroy(struct lexer *lx)
{
    free(lx->token.text);
    lx->token.text = NULL;
}

char *word_literal(const char *s)
{
    char *out = sh_alloc(strlen(s) + 1);
    size_t n = 0;
    int quote = 0;
    for (; *s; s++) {
        if (*s == '\\' && quote != '\'' && s[1]) {
            if (s[1] == '\n') {
                s++;
                continue;
            }
            if (!quote || strchr("$`\"\\", s[1])) {
                out[n++] = *++s;
                continue;
            }
        }
        if (*s == quote) {
            quote = 0;
            continue;
        }
        if (!quote && (*s == '\'' || *s == '"')) {
            quote = *s;
            continue;
        }
        out[n++] = *s;
    }
    return out;
}
