/* mint: a small scripting language interpreter.
 *
 *   x = 1 + 2 * 3            integers (64 bit) and strings ("..")
 *   s = "a" + str(x)         + concatenates strings
 *   if x > 3 { .. } elif x == 3 { .. } else { .. }
 *   while cond { .. }        break, continue
 *   for i in range(0, 10) { .. }
 *   fn add(a, b) { return a + b }
 *   print(a, b, ..)          prints values separated by spaces
 *
 * Builtins: print, len, str, int, substr(s, i, n), ord, chr, readfile,
 * writefile(path, s), input, exit, argc, argv(i). Comments start with #.
 * Values are integers or strings; comparisons and && || ! yield 0 or 1.
 * The interpreter is a recursive descent parser producing a tree that is
 * evaluated directly. Errors print a message with the line and exit 1. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "mint.h"

/* ---- values ---- */

static struct value vint(long i)
{
    struct value v = { V_INT, i, NULL };
    return v;
}

static struct value vstr(const char *s)
{
    struct value v = { V_STR, 0, strdup(s) };
    return v;
}

static struct value vcopy(struct value v)
{
    return v.type == V_STR ? vstr(v.s) : v;
}

static void vfree(struct value *v)
{
    if (v->type == V_STR)
        free(v->s);
    v->s = NULL;
}

static int truthy(struct value v)
{
    return v.type == V_INT ? v.i != 0 : v.s[0] != '\0';
}

/* ---- environment ---- */

struct var {
    char *name;
    struct value val;
    struct var *next;
};

struct scope {
    struct var *vars;
    struct scope *parent;   /* NULL for globals */
};

static struct scope globals;
static struct node *functions[128];
static int nfunctions;
static int script_argc;
static char **script_argv;

static struct var *var_find(struct scope *sc, const char *name)
{
    for (; sc; sc = sc->parent)
        for (struct var *v = sc->vars; v; v = v->next)
            if (strcmp(v->name, name) == 0)
                return v;
    return NULL;
}

static void var_set(struct scope *sc, const char *name, struct value val)
{
    struct var *v = var_find(sc, name);
    if (!v) {
        v = calloc(1, sizeof *v);
        v->name = strdup(name);
        v->next = sc->vars;
        sc->vars = v;
    } else {
        vfree(&v->val);
    }
    v->val = val;
}

static void scope_free(struct scope *sc)
{
    struct var *v = sc->vars;
    while (v) {
        struct var *next = v->next;
        vfree(&v->val);
        free(v->name);
        free(v);
        v = next;
    }
}

/* ---- evaluation ---- */

enum flow { F_NORMAL, F_BREAK, F_CONTINUE, F_RETURN };

static struct value eval(struct node *n, struct scope *sc);
static enum flow exec_block(struct node *b, struct scope *sc, struct value *ret);

static void print_value(struct value v, FILE *f)
{
    if (v.type == V_INT)
        fprintf(f, "%ld", v.i);
    else
        fputs(v.s, f);
}

static struct value call_builtin(struct node *n, struct value *args, int nargs, int *found)
{
    const char *name = n->s;
    *found = 1;
    if (strcmp(name, "print") == 0) {
        for (int i = 0; i < nargs; i++) {
            if (i)
                putchar(' ');
            print_value(args[i], stdout);
        }
        putchar('\n');
        return vint(0);
    }
    if (strcmp(name, "len") == 0 && nargs == 1 && args[0].type == V_STR)
        return vint((long)strlen(args[0].s));
    if (strcmp(name, "str") == 0 && nargs == 1) {
        if (args[0].type == V_STR)
            return vcopy(args[0]);
        char buf[32];
        snprintf(buf, sizeof buf, "%ld", args[0].i);
        return vstr(buf);
    }
    if (strcmp(name, "int") == 0 && nargs == 1)
        return vint(args[0].type == V_INT ? args[0].i : strtol(args[0].s, NULL, 10));
    if (strcmp(name, "substr") == 0 && nargs == 3 && args[0].type == V_STR) {
        long len = (long)strlen(args[0].s);
        long i = args[1].i < 0 ? 0 : args[1].i, cnt = args[2].i;
        if (i > len)
            i = len;
        if (cnt < 0 || i + cnt > len)
            cnt = len - i;
        char *s = malloc((size_t)cnt + 1);
        memcpy(s, args[0].s + i, (size_t)cnt);
        s[cnt] = '\0';
        struct value v = { V_STR, 0, s };
        return v;
    }
    if (strcmp(name, "ord") == 0 && nargs == 1 && args[0].type == V_STR)
        return vint((unsigned char)args[0].s[0]);
    if (strcmp(name, "chr") == 0 && nargs == 1 && args[0].type == V_INT) {
        char buf[2] = { (char)args[0].i, 0 };
        return vstr(buf);
    }
    if (strcmp(name, "readfile") == 0 && nargs == 1 && args[0].type == V_STR) {
        int fd = open(args[0].s, O_RDONLY);
        if (fd < 0)
            fail(n->line, "cannot read", args[0].s);
        size_t cap = 4096, len = 0;
        char *buf = malloc(cap);
        ssize_t r;
        while ((r = read(fd, buf + len, cap - len - 1)) > 0) {
            len += (size_t)r;
            if (len + 1 >= cap)
                buf = realloc(buf, cap *= 2);
        }
        close(fd);
        buf[len] = '\0';
        struct value v = { V_STR, 0, buf };
        return v;
    }
    if (strcmp(name, "writefile") == 0 && nargs == 2 && args[0].type == V_STR && args[1].type == V_STR) {
        int fd = open(args[0].s, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0)
            fail(n->line, "cannot write", args[0].s);
        size_t len = strlen(args[1].s), off = 0;
        while (off < len) {
            ssize_t r = write(fd, args[1].s + off, len - off);
            if (r <= 0)
                fail(n->line, "write failed", args[0].s);
            off += (size_t)r;
        }
        close(fd);
        return vint((long)len);
    }
    if (strcmp(name, "input") == 0 && nargs == 0) {
        char buf[512];
        if (!fgets(buf, sizeof buf, stdin))
            return vstr("");
        size_t l = strlen(buf);
        if (l && buf[l - 1] == '\n')
            buf[l - 1] = '\0';
        return vstr(buf);
    }
    if (strcmp(name, "exit") == 0) {
        fflush(stdout);
        exit(nargs ? (int)args[0].i : 0);
    }
    if (strcmp(name, "argc") == 0)
        return vint(script_argc);
    if (strcmp(name, "argv") == 0 && nargs == 1)
        return vstr(args[0].i >= 0 && args[0].i < script_argc ? script_argv[args[0].i] : "");
    *found = 0;
    return vint(0);
}

static struct value call_function(struct node *n, struct value *args, int nargs, struct scope *sc)
{
    int found;
    struct value r = call_builtin(n, args, nargs, &found);
    if (found)
        return r;
    for (int i = 0; i < nfunctions; i++) {
        struct node *f = functions[i];
        if (strcmp(f->s, n->s) != 0)
            continue;
        struct node *params = f->kids[0];
        if (params->nkids != nargs)
            fail(n->line, "wrong number of arguments for", n->s);
        struct scope local = { NULL, &globals };
        for (int k = 0; k < nargs; k++)
            var_set(&local, params->kids[k]->s, vcopy(args[k]));
        struct value ret = vint(0);
        exec_block(f->kids[1], &local, &ret);
        scope_free(&local);
        return ret;
    }
    fail(n->line, "unknown function", n->s);
}

static struct value eval(struct node *n, struct scope *sc)
{
    switch (n->kind) {
    case N_INT:
        return vint(n->i);
    case N_STR:
        return vstr(n->s);
    case N_VAR: {
        struct var *v = var_find(sc, n->s);
        if (!v)
            fail(n->line, "undefined variable", n->s);
        return vcopy(v->val);
    }
    case N_UNOP: {
        struct value a = eval(n->kids[0], sc);
        if (n->op == T_NOT) {
            int r = !truthy(a);
            vfree(&a);
            return vint(r);
        }
        if (a.type != V_INT)
            fail(n->line, "cannot negate a string", NULL);
        return vint(-a.i);
    }
    case N_BINOP: {
        if (n->op == T_AND || n->op == T_OR) {
            struct value a = eval(n->kids[0], sc);
            int ta = truthy(a);
            vfree(&a);
            if (n->op == T_AND ? !ta : ta)
                return vint(ta);
            struct value b = eval(n->kids[1], sc);
            int tb = truthy(b);
            vfree(&b);
            return vint(tb);
        }
        struct value a = eval(n->kids[0], sc), b = eval(n->kids[1], sc);
        struct value r;
        if (a.type == V_STR || b.type == V_STR) {
            if (n->op == T_PLUS) {
                char ai[32], bi[32];
                const char *as = a.type == V_STR ? a.s : (snprintf(ai, sizeof ai, "%ld", a.i), ai);
                const char *bs = b.type == V_STR ? b.s : (snprintf(bi, sizeof bi, "%ld", b.i), bi);
                char *s = malloc(strlen(as) + strlen(bs) + 1);
                strcpy(s, as);
                strcat(s, bs);
                r.type = V_STR;
                r.i = 0;
                r.s = s;
            } else if (a.type == V_STR && b.type == V_STR &&
                       (n->op == T_EQEQ || n->op == T_NE || n->op == T_LT || n->op == T_GT ||
                        n->op == T_LE || n->op == T_GE)) {
                int c = strcmp(a.s, b.s);
                long v = n->op == T_EQEQ ? c == 0 : n->op == T_NE ? c != 0 : n->op == T_LT ? c < 0 :
                         n->op == T_GT ? c > 0 : n->op == T_LE ? c <= 0 : c >= 0;
                r = vint(v);
            } else {
                fail(n->line, "invalid operation on strings", NULL);
            }
        } else {
            long x = a.i, y = b.i, v;
            switch (n->op) {
            case T_PLUS: v = x + y; break;
            case T_MINUS: v = x - y; break;
            case T_STAR: v = x * y; break;
            case T_SLASH:
            case T_PERCENT:
                if (y == 0)
                    fail(n->line, "division by zero", NULL);
                v = n->op == T_SLASH ? x / y : x % y;
                break;
            case T_EQEQ: v = x == y; break;
            case T_NE: v = x != y; break;
            case T_LT: v = x < y; break;
            case T_LE: v = x <= y; break;
            case T_GT: v = x > y; break;
            case T_GE: v = x >= y; break;
            default: fail(n->line, "bad operator", NULL);
            }
            r = vint(v);
        }
        vfree(&a);
        vfree(&b);
        return r;
    }
    case N_CALL: {
        struct value args[16];
        if (n->nkids > 16)
            fail(n->line, "too many arguments", NULL);
        for (int i = 0; i < n->nkids; i++)
            args[i] = eval(n->kids[i], sc);
        struct value r = call_function(n, args, n->nkids, sc);
        for (int i = 0; i < n->nkids; i++)
            vfree(&args[i]);
        return r;
    }
    default:
        fail(n->line, "not an expression", NULL);
    }
}

static enum flow exec(struct node *n, struct scope *sc, struct value *ret)
{
    switch (n->kind) {
    case N_EXPR: {
        struct value v = eval(n->kids[0], sc);
        vfree(&v);
        return F_NORMAL;
    }
    case N_ASSIGN: {
        struct value v = eval(n->kids[0], sc);
        /* A name already bound in the current scope or in the globals is
         * updated in place; otherwise it becomes a variable of the current
         * scope (local inside a function). */
        struct scope local_only = { sc->vars, NULL };
        struct scope *target = sc;
        if (!var_find(&local_only, n->s) && var_find(&globals, n->s))
            target = &globals;
        var_set(target, n->s, v);
        return F_NORMAL;
    }
    case N_IF: {
        int i = 0;
        for (; i + 1 < n->nkids; i += 2) {
            struct value c = eval(n->kids[i], sc);
            int t = truthy(c);
            vfree(&c);
            if (t)
                return exec_block(n->kids[i + 1], sc, ret);
        }
        if (i < n->nkids)
            return exec_block(n->kids[i], sc, ret);
        return F_NORMAL;
    }
    case N_WHILE:
        for (;;) {
            struct value c = eval(n->kids[0], sc);
            int t = truthy(c);
            vfree(&c);
            if (!t)
                return F_NORMAL;
            enum flow f = exec_block(n->kids[1], sc, ret);
            if (f == F_BREAK)
                return F_NORMAL;
            if (f == F_RETURN)
                return f;
        }
    case N_FOR: {
        struct value a = eval(n->kids[0], sc), b = eval(n->kids[1], sc);
        if (a.type != V_INT || b.type != V_INT)
            fail(n->line, "range needs integers", NULL);
        for (long i = a.i; i < b.i; i++) {
            var_set(sc, n->s, vint(i));
            enum flow f = exec_block(n->kids[2], sc, ret);
            if (f == F_BREAK)
                break;
            if (f == F_RETURN)
                return f;
        }
        return F_NORMAL;
    }
    case N_FN:
        if (nfunctions == 128)
            fail(n->line, "too many functions", NULL);
        functions[nfunctions++] = n;
        return F_NORMAL;
    case N_RETURN:
        vfree(ret);
        *ret = n->nkids ? eval(n->kids[0], sc) : vint(0);
        return F_RETURN;
    case N_BREAK:
        return F_BREAK;
    case N_CONTINUE:
        return F_CONTINUE;
    case N_BLOCK:
        return exec_block(n, sc, ret);
    default:
        fail(n->line, "bad statement", NULL);
    }
}

static enum flow exec_block(struct node *b, struct scope *sc, struct value *ret)
{
    for (int i = 0; i < b->nkids; i++) {
        enum flow f = exec(b->kids[i], sc, ret);
        if (f != F_NORMAL)
            return f;
    }
    return F_NORMAL;
}

static char *read_all(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return NULL;
    size_t cap = 4096, len = 0;
    char *buf = malloc(cap);
    size_t r;
    while ((r = fread(buf + len, 1, cap - len - 1, f)) > 0) {
        len += r;
        if (len + 1 >= cap)
            buf = realloc(buf, cap *= 2);
    }
    fclose(f);
    buf[len] = '\0';
    return buf;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: mint script [args...]\n");
        return 2;
    }
    char *text = read_all(argv[1]);
    if (!text) {
        fprintf(stderr, "mint: %s: %s\n", argv[1], strerror(errno));
        return 1;
    }
    script_argc = argc - 1;
    script_argv = argv + 1;
    struct node *program = mint_parse(text);
    struct value ret = vint(0);
    exec_block(program, &globals, &ret);
    fflush(stdout);
    return 0;
}
