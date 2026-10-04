#include <glob.h>
#include <fnmatch.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

static int add(glob_t *g, const char *path, int mark)
{
    size_t n = strlen(path), off = g->gl_offs;
    char *copy = malloc(n + 2);
    if (!copy)
        return GLOB_NOSPACE;
    memcpy(copy, path, n);
    if (mark && n && path[n - 1] != '/')
        copy[n++] = '/';
    copy[n] = 0;
    char **v = realloc(g->gl_pathv, (off + g->gl_pathc + 2) * sizeof *v);
    if (!v) {
        free(copy);
        return GLOB_NOSPACE;
    }
    if (!g->gl_pathv)
        for (size_t i = 0; i < off; i++)
            v[i] = NULL;
    g->gl_pathv = v;
    v[off + g->gl_pathc++] = copy;
    v[off + g->gl_pathc] = NULL;
    return 0;
}

static int walk(const char *base, const char *pattern, int flags, int (*errfunc)(const char *, int),
                glob_t *g, unsigned depth)
{
    if (depth > 256)
        return GLOB_NOSPACE;
    if (!*pattern) {
        struct stat st;
        if (stat(*base ? base : ".", &st))
            return 0;
        return add(g, base, (flags & GLOB_MARK) && S_ISDIR(st.st_mode));
    }
    const char *slash = strchr(pattern, '/');
    size_t n = slash ? (size_t)(slash - pattern) : strlen(pattern);
    char *part = malloc(n + 1);
    if (!part)
        return GLOB_NOSPACE;
    memcpy(part, pattern, n);
    part[n] = 0;
    DIR *dir = opendir(*base ? base : ".");
    if (!dir) {
        int e = errno;
        free(part);
        if (e == ENOENT || e == ENOTDIR)
            return 0;
        return ((errfunc && errfunc(*base ? base : ".", e)) || (flags & GLOB_ERR)) ? GLOB_ABORTED : 0;
    }
    int result = 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (fnmatch(part, entry->d_name, FNM_PERIOD | ((flags & GLOB_NOESCAPE) ? FNM_NOESCAPE : 0)))
            continue;
        size_t b = strlen(base), e = strlen(entry->d_name);
        char *path = malloc(b + e + 3);
        if (!path) {
            result = GLOB_NOSPACE;
            break;
        }
        memcpy(path, base, b);
        if (b && path[b - 1] != '/')
            path[b++] = '/';
        memcpy(path + b, entry->d_name, e + 1);
        if (slash) {
            struct stat st;
            if (stat(path, &st) || !S_ISDIR(st.st_mode)) {
                free(path);
                continue;
            }
            const char *rest = slash + 1;
            while (*rest == '/')
                rest++;
            if (!*rest) {
                path[b + e] = '/';
                path[b + e + 1] = 0;
            }
            result = walk(path, rest, flags, errfunc, g, depth + 1);
        } else
            result = walk(path, "", flags, errfunc, g, depth + 1);
        free(path);
        if (result)
            break;
    }
    closedir(dir);
    free(part);
    return result;
}

static int compare(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int glob(const char *pattern, int flags, int (*errfunc)(const char *, int), glob_t *g)
{
    if (!(flags & GLOB_APPEND)) {
        g->gl_pathc = 0;
        g->gl_pathv = NULL;
    }
    if (!(flags & GLOB_DOOFFS))
        g->gl_offs = 0;
    size_t before = g->gl_pathc;
    const char *rest = pattern, *base = "";
    if (*rest == '/') {
        base = "/";
        while (*rest == '/')
            rest++;
    }
    int r = *pattern ? walk(base, rest, flags, errfunc, g, 0) : 0;
    if (!r && g->gl_pathc == before)
        r = flags & GLOB_NOCHECK ? add(g, pattern, 0) : GLOB_NOMATCH;
    if (!r && !(flags & GLOB_NOSORT))
        qsort(g->gl_pathv + g->gl_offs + before, g->gl_pathc - before, sizeof(char *), compare);
    return r;
}

void globfree(glob_t *g)
{
    if (g->gl_pathv) {
        for (size_t i = 0; i < g->gl_pathc; i++)
            free(g->gl_pathv[g->gl_offs + i]);
        free(g->gl_pathv);
    }
    g->gl_pathv = NULL;
    g->gl_pathc = 0;
}
