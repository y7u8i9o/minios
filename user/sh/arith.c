#include "sh.h"
#include <limits.h>

struct arithmetic {
    const char *p;
    int error, depth;
};
static long expression(struct arithmetic *a, int minimum, int evaluate);
static void spaces(struct arithmetic *a)
{
    while (isspace((unsigned char)*a->p))
        a->p++;
}
static long primary(struct arithmetic *a, int evaluate)
{
    spaces(a);
    if (++a->depth > 64) {
        a->error = 1;
        a->depth--;
        return 0;
    }
    long v = 0;
    if (strchr("+-!~", *a->p) && *a->p) {
        char op = *a->p++;
        v = primary(a, evaluate);
        if (op == '-')
            v = (long)(0ul - (unsigned long)v);
        if (op == '!')
            v = !v;
        if (op == '~')
            v = ~v;
    } else if (*a->p == '(') {
        a->p++;
        v = expression(a, 1, evaluate);
        spaces(a);
        if (*a->p == ')')
            a->p++;
        else
            a->error = 1;
    } else if (isdigit((unsigned char)*a->p)) {
        char *end;
        v = strtol(a->p, &end, 0);
        a->p = end;
    } else if (isalpha((unsigned char)*a->p) || *a->p == '_') {
        const char *start = a->p++;
        while (isalnum((unsigned char)*a->p) || *a->p == '_')
            a->p++;
        char *name = sh_slice(start, (size_t)(a->p - start));
        const char *s = var_get(name);
        if (evaluate && s && *s) {
            struct arithmetic inner = {.p = s, .depth = a->depth};
            v = expression(&inner, 1, 1);
            spaces(&inner);
            if (inner.error || *inner.p)
                a->error = 1;
        }
        free(name);
    } else {
        a->error = 1;
        if (*a->p)
            a->p++;
    }
    a->depth--;
    return v;
}
struct operator {
    const char *s;
    int precedence;
};
static const struct operator ops[] = {{"||", 2}, {"&&", 3}, {"|", 4},  {"^", 5},  {"&", 6},
                                      {"==", 7}, {"!=", 7}, {"<=", 8}, {">=", 8}, {"<<", 9},
                                      {">>", 9}, {"<", 8},  {">", 8},  {"+", 10}, {"-", 10},
                                      {"*", 11}, {"/", 11}, {"%", 11}, {NULL, 0}};
static long expression(struct arithmetic *a, int minimum, int evaluate)
{
    spaces(a);
    /* Assignment is right associative and restricted to variable lvalues. */
    const char *saved = a->p;
    if (minimum == 1 && (isalpha((unsigned char)*a->p) || *a->p == '_')) {
        const char *end = ++a->p;
        while (isalnum((unsigned char)*a->p) || *a->p == '_')
            a->p++;
        end = a->p;
        spaces(a);
        if (*a->p == '=' && a->p[1] != '=') {
            a->p++;
            long v = expression(a, 1, evaluate);
            if (evaluate && !a->error) {
                char *name = sh_slice(saved, (size_t)(end - saved)), value[64];
                snprintf(value, sizeof value, "%ld", v);
                var_set(name, value, 0);
                free(name);
            }
            return v;
        }
        a->p = saved;
    }
    long left = primary(a, evaluate);
    while (!a->error) {
        spaces(a);
        const struct operator *op = NULL;
        for (int i = 0; ops[i].s; i++)
            if (!strncmp(a->p, ops[i].s, strlen(ops[i].s))) {
                op = &ops[i];
                break;
            }
        if (!op || op->precedence < minimum)
            break;
        a->p += strlen(op->s);
        int active = evaluate;
        if (!strcmp(op->s, "&&") && !left)
            active = 0;
        if (!strcmp(op->s, "||") && left)
            active = 0;
        long right = expression(a, op->precedence + 1, active);
        if (!evaluate)
            continue;
        const char *s = op->s;
        if (!strcmp(s, "||"))
            left = left || right;
        else if (!strcmp(s, "&&"))
            left = left && right;
        else if (!strcmp(s, "=="))
            left = left == right;
        else if (!strcmp(s, "!="))
            left = left != right;
        else if (!strcmp(s, "<="))
            left = left <= right;
        else if (!strcmp(s, ">="))
            left = left >= right;
        else if (!strcmp(s, "<<") || !strcmp(s, ">>")) {
            if (right < 0 || right >= (long)(sizeof(long) * 8)) {
                a->error = 1;
                break;
            }
            left = s[0] == '<' ? (long)((unsigned long)left << right) : left >> right;
        } else
            switch (*s) {
            case '+':
                left = (long)((unsigned long)left + (unsigned long)right);
                break;
            case '-':
                left = (long)((unsigned long)left - (unsigned long)right);
                break;
            case '*':
                left = (long)((unsigned long)left * (unsigned long)right);
                break;
            case '/':
            case '%':
                if (!right || (left == LONG_MIN && right == -1)) {
                    a->error = 1;
                    break;
                }
                left = *s == '/' ? left / right : left % right;
                break;
            case '<':
                left = left < right;
                break;
            case '>':
                left = left > right;
                break;
            case '&':
                left &= right;
                break;
            case '|':
                left |= right;
                break;
            case '^':
                left ^= right;
                break;
            }
    }
    if (minimum == 1 && *a->p == '?' && !a->error) {
        a->p++;
        long yes = expression(a, 1, evaluate && left);
        spaces(a);
        if (*a->p != ':')
            a->error = 1;
        else {
            a->p++;
            long no = expression(a, 1, evaluate && !left);
            left = left ? yes : no;
        }
    }
    return left;
}
long arith_eval(const char *text, int *error)
{
    struct arithmetic a = {.p = text};
    long value = expression(&a, 1, 1);
    spaces(&a);
    *error = a.error || *a.p;
    return value;
}
