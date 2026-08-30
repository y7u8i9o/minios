/* expr: evaluate an integer expression given as arguments or as one
 * string: + - * / % ( ) and the comparisons = != < <= > >=. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

static const char *p;
static int err;

static long parse_cmp(void);

static void skip(void) { while (*p == ' ') p++; }

static long parse_primary(void)
{
    skip();
    if (*p == '(') {
        p++;
        long v = parse_cmp();
        skip();
        if (*p == ')') p++; else err = 1;
        return v;
    }
    if (*p == '-') {
        p++;
        return -parse_primary();
    }
    if (!isdigit((unsigned char)*p)) {
        err = 1;
        return 0;
    }
    return strtol(p, (char **)&p, 10);
}

static long parse_mul(void)
{
    long v = parse_primary();
    for (;;) {
        skip();
        char op = *p;
        if (op != '*' && op != '/' && op != '%')
            return v;
        p++;
        long r = parse_primary();
        if ((op == '/' || op == '%') && r == 0) {
            err = 2;
            return 0;
        }
        v = op == '*' ? v * r : op == '/' ? v / r : v % r;
    }
}

static long parse_add(void)
{
    long v = parse_mul();
    for (;;) {
        skip();
        char op = *p;
        if (op != '+' && op != '-')
            return v;
        p++;
        long r = parse_mul();
        v = op == '+' ? v + r : v - r;
    }
}

static long parse_cmp(void)
{
    long v = parse_add();
    skip();
    if (p[0] == '=' ) { p++; return v == parse_add(); }
    if (p[0] == '!' && p[1] == '=') { p += 2; return v != parse_add(); }
    if (p[0] == '<' && p[1] == '=') { p += 2; return v <= parse_add(); }
    if (p[0] == '>' && p[1] == '=') { p += 2; return v >= parse_add(); }
    if (p[0] == '<') { p++; return v < parse_add(); }
    if (p[0] == '>') { p++; return v > parse_add(); }
    return v;
}

int main(int argc, char **argv)
{
    char buf[1024] = "";
    for (int i = 1; i < argc; i++) {
        strncat(buf, argv[i], sizeof buf - strlen(buf) - 2);
        strcat(buf, " ");
    }
    p = buf;
    long v = parse_cmp();
    skip();
    if (*p)
        err = 1;
    if (err == 2) {
        fprintf(stderr, "expr: division by zero\n");
        return 2;
    }
    if (err) {
        fprintf(stderr, "expr: syntax error\n");
        return 2;
    }
    printf("%ld\n", v);
    return v == 0 ? 1 : 0;
}
