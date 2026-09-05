#include "sh.h"

struct variable {
    char *name, *value;
    int exported, level;
    struct variable *next;
};
struct definition {
    char *name, *value;
    struct node *body;
    struct definition *next;
};
static struct variable *variables;
static struct definition *aliases, *functions;
static int level;

int valid_name(const char *s)
{
    if (!s || (!isalpha((unsigned char)*s) && *s != '_'))
        return 0;
    for (s++; *s; s++)
        if (!isalnum((unsigned char)*s) && *s != '_')
            return 0;
    return 1;
}
int assignment(const char *s)
{
    const char *eq = strchr(s, '=');
    if (!eq)
        return 0;
    char *name = sh_slice(s, (size_t)(eq - s));
    int yes = valid_name(name);
    free(name);
    return yes;
}
static struct variable *find(const char *name)
{
    for (struct variable *v = variables; v; v = v->next)
        if (!strcmp(v->name, name))
            return v;
    return NULL;
}
const char *var_get(const char *name)
{
    struct variable *v = find(name);
    return v ? v->value : NULL;
}
int var_set(const char *name, const char *value, int exported)
{
    if (!valid_name(name)) {
        fprintf(stderr, "sh: %s: invalid name\n", name);
        return 1;
    }
    struct variable *v = find(name);
    if (!v) {
        v = sh_alloc(sizeof *v);
        v->name = strdup(name);
        v->next = variables;
        variables = v;
    }
    char *copy = strdup(value);
    free(v->value);
    v->value = copy;
    if (exported)
        v->exported = 1;
    if (v->exported)
        setenv(name, value, 1);
    return 0;
}
int var_local(const char *name, const char *value)
{
    if (!valid_name(name))
        return 1;
    struct variable *old = find(name);
    if (old && old->level == level)
        return var_set(name, value, 0);
    struct variable *v = sh_alloc(sizeof *v);
    v->name = strdup(name);
    v->value = strdup(value);
    v->level = level;
    v->exported = old ? old->exported : 0;
    v->next = variables;
    variables = v;
    if (v->exported)
        setenv(name, value, 1);
    return 0;
}
void scope_push(void)
{
    level++;
}
void scope_pop(void)
{
    struct variable **p = &variables;
    while (*p) {
        struct variable *v = *p;
        if (v->level != level) {
            p = &v->next;
            continue;
        }
        *p = v->next;
        struct variable *old = find(v->name);
        if (old && old->exported)
            setenv(old->name, old->value, 1);
        else
            unsetenv(v->name);
        free(v->name);
        free(v->value);
        free(v);
    }
    if (level)
        level--;
}
void var_unset(const char *name)
{
    struct variable **p = &variables;
    while (*p) {
        struct variable *v = *p;
        if (!strcmp(v->name, name)) {
            /* Keep a local tombstone so scope_pop restores the outer binding. */
            if (v->level) {
                free(v->value);
                v->value = NULL;
                v->exported = 0;
            } else {
                *p = v->next;
                free(v->name);
                free(v->value);
                free(v);
            }
            break;
        }
        p = &v->next;
    }
    unsetenv(name);
}
void vars_init(void)
{
    for (char **e = environ; e && *e; e++) {
        const char *eq = strchr(*e, '=');
        if (!eq)
            continue;
        struct variable *v = sh_alloc(sizeof *v);
        v->name = sh_slice(*e, (size_t)(eq - *e));
        v->value = strdup(eq + 1);
        v->exported = 1;
        v->next = variables;
        variables = v;
    }
}
void vars_print(void)
{
    for (struct variable *v = variables; v; v = v->next)
        if (find(v->name) == v && v->value)
            printf("%s=%s\n", v->name, v->value);
}
const char *var_lookup(const char *name, char *tmp, size_t size)
{
    if (!strcmp(name, "?")) {
        snprintf(tmp, size, "%d", last_status);
        return tmp;
    }
    if (!strcmp(name, "$")) {
        snprintf(tmp, size, "%d", getpid());
        return tmp;
    }
    if (!strcmp(name, "#")) {
        snprintf(tmp, size, "%d", script_argc > 0 ? script_argc - 1 : 0);
        return tmp;
    }
    if (!strcmp(name, "@") || !strcmp(name, "*")) {
        size_t n = 0;
        const char *ifs = var_get("IFS");
        char sep = ifs ? *ifs : ' ';
        for (int i = 1; i < script_argc; i++) {
            if (i > 1 && sep && n + 1 < size)
                tmp[n++] = sep;
            for (const char *s = script_argv[i]; *s && n + 1 < size; s++)
                tmp[n++] = *s;
        }
        tmp[n] = 0;
        return tmp;
    }
    if (isdigit((unsigned char)*name)) {
        char *end;
        long i = strtol(name, &end, 10);
        if (!*end)
            return i >= 0 && i < script_argc ? script_argv[i] : "";
    }
    const char *v = var_get(name);
    return v ? v : "";
}
static struct definition *def_find(struct definition *d, const char *name)
{
    for (; d; d = d->next)
        if (!strcmp(d->name, name))
            return d;
    return NULL;
}
const char *alias_get(const char *name)
{
    struct definition *d = def_find(aliases, name);
    return d ? d->value : NULL;
}
int alias_set(const char *name, const char *value)
{
    struct definition *d = def_find(aliases, name);
    if (!d) {
        d = sh_alloc(sizeof *d);
        d->name = strdup(name);
        d->next = aliases;
        aliases = d;
    }
    free(d->value);
    d->value = strdup(value);
    return 0;
}
void alias_unset(const char *name)
{
    struct definition **p = &aliases;
    while (*p) {
        struct definition *d = *p;
        if (!strcmp(d->name, name)) {
            *p = d->next;
            free(d->name);
            free(d->value);
            free(d);
            return;
        }
        p = &d->next;
    }
}
void alias_print(void)
{
    for (struct definition *d = aliases; d; d = d->next)
        printf("alias %s='%s'\n", d->name, d->value);
}
struct node *function_get(const char *name)
{
    struct definition *d = def_find(functions, name);
    return d ? d->body : NULL;
}
void function_set(const char *name, const struct node *body)
{
    struct definition *d = def_find(functions, name);
    if (!d) {
        d = sh_alloc(sizeof *d);
        d->name = strdup(name);
        d->next = functions;
        functions = d;
    }
    node_free(d->body);
    d->body = node_clone(body);
}
