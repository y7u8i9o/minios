#pragma once
/* Shared definitions of the mint interpreter: values, tokens and the
 * syntax tree built by mint_parse.c and evaluated by mint.c. */
#include <stddef.h>

enum vtype { V_INT, V_STR };

struct value {
    enum vtype type;
    long i;
    char *s;        /* owned */
};

enum tok {
    T_EOF, T_NL, T_INT, T_STR, T_IDENT,
    T_LP, T_RP, T_LB, T_RB, T_COMMA,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT,
    T_EQ, T_EQEQ, T_NE, T_LT, T_LE, T_GT, T_GE, T_AND, T_OR, T_NOT,
    T_IF, T_ELIF, T_ELSE, T_WHILE, T_FOR, T_IN, T_FN, T_RETURN, T_BREAK, T_CONTINUE,
};

enum nkind {
    N_INT, N_STR, N_VAR, N_BINOP, N_UNOP, N_CALL, N_ASSIGN, N_IF, N_WHILE, N_FOR,
    N_BLOCK, N_FN, N_RETURN, N_BREAK, N_CONTINUE, N_EXPR,
};

struct node {
    enum nkind kind;
    int line;
    enum tok op;
    long i;
    char *s;
    struct node **kids;
    int nkids;
};

/* mint_parse.c */
__attribute__((noreturn)) void fail(int line, const char *msg, const char *detail);
struct node *mint_parse(const char *text);
