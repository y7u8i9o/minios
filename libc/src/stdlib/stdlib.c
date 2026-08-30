#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>

#define LONG_MAX_PLUS_ONE (1UL << 63)

int atoi(const char *s)
{
    return (int)strtol(s, NULL, 10);
}

long atol(const char *s)
{
    return strtol(s, NULL, 10);
}

/* Parse sign, optional base prefix and digits. Returns the magnitude,
 * sets *neg and *end. */
static unsigned long parse_ul(const char *s, char **end, int base, int *neg)
{
    const char *p = s;
    while (isspace(*p))
        p++;
    *neg = 0;
    if (*p == '+' || *p == '-') {
        *neg = *p == '-';
        p++;
    }
    if ((base == 0 || base == 16) && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        base = 16;
        p += 2;
    } else if (base == 0 && p[0] == '0') {
        base = 8;
    } else if (base == 0) {
        base = 10;
    }
    unsigned long v = 0;
    const char *start = p;
    for (;; p++) {
        int d;
        if (isdigit(*p))
            d = *p - '0';
        else if (isalpha(*p))
            d = tolower(*p) - 'a' + 10;
        else
            break;
        if (d >= base)
            break;
        if (v > (~0UL - (unsigned long)d) / (unsigned long)base) {
            errno = ERANGE;
            v = ~0UL;
        } else {
            v = v * (unsigned long)base + (unsigned long)d;
        }
    }
    if (end)
        *end = (char *)(p == start ? s : p);
    return v;
}

unsigned long strtoul(const char *s, char **end, int base)
{
    int neg;
    unsigned long v = parse_ul(s, end, base, &neg);
    return neg ? (unsigned long)-(long)v : v;
}

long strtol(const char *s, char **end, int base)
{
    int neg;
    unsigned long v = parse_ul(s, end, base, &neg);
    if (neg) {
        if (v > LONG_MAX_PLUS_ONE) {
            errno = ERANGE;
            return (long)-LONG_MAX_PLUS_ONE;
        }
        return v == LONG_MAX_PLUS_ONE ? (long)-LONG_MAX_PLUS_ONE : -(long)v;
    }
    if (v > LONG_MAX_PLUS_ONE - 1) {
        errno = ERANGE;
        return (long)(LONG_MAX_PLUS_ONE - 1);
    }
    return (long)v;
}

int abs(int v)
{
    return v < 0 ? -v : v;
}

long labs(long v)
{
    return v < 0 ? -v : v;
}

char *getenv(const char *name)
{
    size_t n = strlen(name);
    if (!environ)
        return NULL;
    for (char **e = environ; *e; e++) {
        if (strncmp(*e, name, n) == 0 && (*e)[n] == '=')
            return *e + n + 1;
    }
    return NULL;
}

/* Environment updates allocate a new vector; the old one is not freed when
 * it came from the kernel supplied stack. */
int setenv(const char *name, const char *value, int overwrite)
{
    if (getenv(name) && !overwrite)
        return 0;
    size_t count = 0;
    while (environ && environ[count])
        count++;
    char **nv = malloc((count + 2) * sizeof(char *));
    if (!nv)
        return -1;
    size_t n = strlen(name);
    size_t k = 0;
    for (size_t i = 0; i < count; i++) {
        if (strncmp(environ[i], name, n) == 0 && environ[i][n] == '=')
            continue;
        nv[k++] = environ[i];
    }
    char *entry = malloc(n + 1 + strlen(value) + 1);
    if (!entry) {
        free(nv);
        return -1;
    }
    strcpy(entry, name);
    entry[n] = '=';
    strcpy(entry + n + 1, value);
    nv[k++] = entry;
    nv[k] = NULL;
    environ = nv;
    return 0;
}

static unsigned rand_state = 1;

int rand(void)
{
    rand_state = rand_state * 1103515245u + 12345u;
    return (int)((rand_state >> 1) & RAND_MAX);
}

void srand(unsigned seed)
{
    rand_state = seed;
}

static void swap_bytes(char *a, char *b, size_t n)
{
    while (n--) {
        char t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

/* Insertion sort: simple and adequate for the small arrays used here. */
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    char *b = base;
    for (size_t i = 1; i < n; i++) {
        for (size_t j = i; j > 0 && cmp(b + (j - 1) * size, b + j * size) > 0; j--)
            swap_bytes(b + (j - 1) * size, b + j * size, size);
    }
}

int unsetenv(const char *name)
{
    size_t n = strlen(name);
    size_t k = 0;
    for (size_t i = 0; environ && environ[i]; i++) {
        if (strncmp(environ[i], name, n) == 0 && environ[i][n] == '=')
            continue;
        environ[k++] = environ[i];
    }
    if (environ)
        environ[k] = NULL;
    return 0;
}
