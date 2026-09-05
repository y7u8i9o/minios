#include <fnmatch.h>
#include <ctype.h>
#include <string.h>

static int fold(int c, int flags)
{
    return flags & FNM_CASEFOLD ? tolower((unsigned char)c) : (unsigned char)c;
}

static int class_match(const char *name, size_t n, int c)
{
#define CLASS(s, f) if (n == sizeof(s)-1 && !strncmp(name, s, n)) return f((unsigned char)c)
    if (n == 5 && !strncmp(name, "blank", n)) return c == ' ' || c == '\t';
    if (n == 5 && !strncmp(name, "graph", n)) return isprint(c) && c != ' ';
    CLASS("alnum", isalnum); CLASS("alpha", isalpha);
    CLASS("cntrl", iscntrl); CLASS("digit", isdigit);
    CLASS("lower", islower); CLASS("print", isprint); CLASS("punct", ispunct);
    CLASS("space", isspace); CLASS("upper", isupper); CLASS("xdigit", isxdigit);
#undef CLASS
    return 0;
}

/* Return -1 for an unterminated bracket, which represents a literal '['. */
static int bracket(const char **pattern, int c, int flags)
{
    const char *p = *pattern;
    int invert = *p == '!' || *p == '^', match = 0, first = 1;
    if (invert) p++;
    while (*p && (*p != ']' || first)) {
        first = 0;
        if (p[0] == '[' && p[1] == ':') {
            const char *end = strstr(p + 2, ":]");
            if (end) {
                match |= class_match(p + 2, (size_t)(end - p - 2), c);
                p = end + 2;
                continue;
            }
        }
        if (*p == '\\' && !(flags & FNM_NOESCAPE) && p[1]) p++;
        int low = fold(*p++, flags), high = low;
        if (*p == '-' && p[1] && p[1] != ']') {
            p++;
            if (*p == '\\' && !(flags & FNM_NOESCAPE) && p[1]) p++;
            high = fold(*p++, flags);
        }
        int value = fold(c, flags);
        if (value >= low && value <= high) match = 1;
    }
    if (*p != ']') return -1;
    *pattern = p + 1;
    return match != invert;
}

int fnmatch(const char *p, const char *s, int flags)
{
    const char *start = s, *star = NULL, *retry = NULL;
    while (*s) {
        int leading = (flags & FNM_PERIOD) && *s == '.' &&
            (s == start || ((flags & FNM_PATHNAME) && s[-1] == '/'));
        if (*p == '*' && !leading) {
            while (*p == '*') p++;
            star = p; retry = s;
            continue;
        }
        const char *next = p;
        int ok = 0;
        if (*next == '?' && !leading) {
            next++;
            ok = !(flags & FNM_PATHNAME) || *s != '/';
        } else if (*next == '[' && !leading && (!(flags & FNM_PATHNAME) || *s != '/')) {
            next++;
            int result = bracket(&next, (unsigned char)*s, flags);
            if (result < 0) { next = p + 1; ok = *s == '['; }
            else ok = result;
        } else if (*next && *next != '*') {
            if (*next == '\\' && !(flags & FNM_NOESCAPE) && next[1]) next++;
            ok = fold(*next++, flags) == fold(*s, flags);
        }
        if (ok) {
            if ((flags & FNM_PATHNAME) && *s == '/') star = NULL;
            p = next; s++; continue;
        }
        if (!star || !*retry || ((flags & FNM_PATHNAME) && *retry == '/') || leading)
            return FNM_NOMATCH;
        s = ++retry; p = star;
    }
    while (*p == '*') p++;
    return *p ? FNM_NOMATCH : 0;
}
