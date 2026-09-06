#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>

#define LONG_MAX_PLUS_ONE (1UL << 63)

int atoi(const char *s)
{
    return (int)strtol(s, NULL, 10);
}

long atol(const char *s)
{
    return strtol(s, NULL, 10);
}

static int ascii_equal_fold(const char *s, const char *word)
{
    while (*word) {
        char c = *s++;
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
        if (c != *word++)
            return 0;
    }
    return 1;
}

/* Scale by a decimal exponent without requiring the transcendental libm
 * functions. Decimal input is rounded once when it is converted to double. */
static double scale_decimal(double value, int exp10)
{
    static const double powers[] = {
        1e1, 1e2, 1e4, 1e8, 1e16, 1e32, 1e64, 1e128, 1e256
    };
    unsigned exp;
    if (exp10 < 0)
        exp = exp10 < -511 ? 512U : (unsigned)-exp10;
    else
        exp = exp10 > 511 ? 512U : (unsigned)exp10;

    for (unsigned bit = 0; exp && bit < sizeof(powers) / sizeof(powers[0]); bit++) {
        if (exp & 1U)
            value = exp10 < 0 ? value / powers[bit] : value * powers[bit];
        exp >>= 1;
    }
    if (exp)
        return exp10 < 0 ? 0.0 : __builtin_huge_val();
    return value;
}

static int hex_digit_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

static double scale_binary(double value, long long exponent)
{
    if (exponent > 2048)
        return __builtin_huge_val();
    if (exponent < -2048)
        return 0.0;
    while (exponent > 0) {
        int step = exponent > 512 ? 512 : (int)exponent;
        value *= step == 512 ? 0x1p512 : (union { uint64_t bits; double value; }){
            .bits = (uint64_t)(step + 1023) << 52
        }.value;
        exponent -= step;
    }
    while (exponent < 0) {
        int step = exponent < -512 ? -512 : (int)exponent;
        value *= step == -512 ? 0x1p-512 : (union { uint64_t bits; double value; }){
            .bits = (uint64_t)(step + 1023) << 52
        }.value;
        exponent -= step;
    }
    return value;
}

static double parse_hex_double(const char *s, const char **parsed_end)
{
    const char *p = s + 2;
    uint64_t mantissa = 0;
    long long fractional_digits = 0, dropped = 0;
    int kept = 0, after_point = 0, first_dropped = -1;
    for (;;) {
        if (*p == '.' && !after_point) {
            after_point = 1;
            p++;
            continue;
        }
        int digit = hex_digit_value(*p);
        if (digit < 0)
            break;
        p++;
        if (after_point && fractional_digits < 1000000)
            fractional_digits++;
        if (kept == 0 && digit == 0)
            continue;
        if (kept < 15) {
            mantissa = mantissa * 16U + (unsigned)digit;
            kept++;
        } else {
            if (first_dropped < 0)
                first_dropped = digit;
            if (dropped < 1000000)
                dropped++;
        }
    }

    long long explicit_exponent = 0;
    if (*p == 'p' || *p == 'P') {
        const char *exponent_mark = p++;
        int negative = 0;
        if (*p == '+' || *p == '-') {
            negative = *p == '-';
            p++;
        }
        if (!isdigit((unsigned char)*p)) {
            p = exponent_mark;
        } else {
            while (isdigit((unsigned char)*p)) {
                if (explicit_exponent < 1000000)
                    explicit_exponent = explicit_exponent * 10 + (*p - '0');
                p++;
            }
            if (negative)
                explicit_exponent = -explicit_exponent;
        }
    }
    *parsed_end = p;
    if (first_dropped >= 8)
        mantissa++;
    long long exponent = explicit_exponent - 4 * fractional_digits + 4 * dropped;
    double value = scale_binary((double)mantissa, exponent);
    if (mantissa && (__builtin_isinf(value) || value == 0.0))
        errno = ERANGE;
    return value;
}

double strtod(const char *s, char **end)
{
    const char *original = s;
    while (isspace((unsigned char)*s))
        s++;

    int negative = 0;
    if (*s == '+' || *s == '-') {
        negative = *s == '-';
        s++;
    }

    if (ascii_equal_fold(s, "inf")) {
        s += 3;
        if (ascii_equal_fold(s, "inity"))
            s += 5;
        if (end)
            *end = (char *)s;
        double value = __builtin_huge_val();
        return negative ? -value : value;
    }
    if (ascii_equal_fold(s, "nan")) {
        s += 3;
        if (*s == '(') {
            const char *payload = s + 1;
            while (isalnum((unsigned char)*payload) || *payload == '_')
                payload++;
            if (*payload == ')')
                s = payload + 1;
        }
        if (end)
            *end = (char *)s;
        double value = __builtin_nan("");
        return negative ? -value : value;
    }

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X') &&
        (hex_digit_value(s[2]) >= 0 || (s[2] == '.' && hex_digit_value(s[3]) >= 0))) {
        const char *parsed_end;
        double value = parse_hex_double(s, &parsed_end);
        if (end)
            *end = (char *)parsed_end;
        return negative ? -value : value;
    }

    uint64_t mantissa = 0;
    int kept = 0, dropped = 0, decimal_digits = 0;
    int first_dropped = -1, seen_digit = 0, after_point = 0;
    for (;;) {
        if (*s == '.' && !after_point) {
            after_point = 1;
            s++;
            continue;
        }
        if (!isdigit((unsigned char)*s))
            break;
        int digit = *s++ - '0';
        seen_digit = 1;
        if (after_point)
            decimal_digits++;
        if (kept == 0 && digit == 0)
            continue;
        if (kept < 18) {
            mantissa = mantissa * 10U + (unsigned)digit;
            kept++;
        } else {
            if (first_dropped < 0)
                first_dropped = digit;
            dropped++;
        }
    }
    if (!seen_digit) {
        if (end)
            *end = (char *)original;
        return 0.0;
    }

    int explicit_exp = 0;
    if (*s == 'e' || *s == 'E') {
        const char *exponent_mark = s++;
        int exponent_negative = 0;
        if (*s == '+' || *s == '-') {
            exponent_negative = *s == '-';
            s++;
        }
        if (!isdigit((unsigned char)*s)) {
            s = exponent_mark;
        } else {
            while (isdigit((unsigned char)*s)) {
                if (explicit_exp < 100000)
                    explicit_exp = explicit_exp * 10 + (*s - '0');
                s++;
            }
            if (exponent_negative)
                explicit_exp = -explicit_exp;
        }
    }
    if (end)
        *end = (char *)s;

    if (first_dropped >= 5)
        mantissa++;
    int exp10 = explicit_exp - decimal_digits + dropped;
    double value = scale_decimal((double)mantissa, exp10);
    if (mantissa && (__builtin_isinf(value) || value == 0.0))
        errno = ERANGE;
    return negative ? -value : value;
}

double atof(const char *s)
{
    return strtod(s, NULL);
}

float strtof(const char *s, char **end)
{
    double value = strtod(s, end);
    float narrowed = (float)value;
    if (__builtin_isfinite(value) &&
        ((__builtin_isinf(narrowed)) || (narrowed == 0.0f && value != 0.0)))
        errno = ERANGE;
    return narrowed;
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

/* Runs command through /bin/sh -c and returns its wait status. With a
 * null command reports that a shell is available. Interrupt and quit
 * are ignored in the parent while the child runs, as POSIX requires,
 * so that control C reaches only the command. */
int system(const char *command)
{
    if (!command)
        return 1;
    sighandler_t old_int = signal(SIGINT, SIG_IGN);
    sighandler_t old_quit = signal(SIGQUIT, SIG_IGN);
    int status = -1;
    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGINT, old_int);
        signal(SIGQUIT, old_quit);
        char *const argv[] = { "sh", "-c", (char *)command, NULL };
        execv("/bin/sh", argv);
        _exit(127);
    }
    if (pid > 0) {
        while (waitpid(pid, &status, 0) < 0) {
            if (errno != EINTR) {
                status = -1;
                break;
            }
        }
    }
    signal(SIGINT, old_int);
    signal(SIGQUIT, old_quit);
    return status;
}

/* Replaces the trailing XXXXXX of template with letters and creates the
 * file exclusively, retrying on a collision. Returns the descriptor or
 * -1 with errno set. */
int mkstemp(char *template)
{
    size_t len = strlen(template);
    if (len < 6 || strcmp(template + len - 6, "XXXXXX") != 0) {
        errno = EINVAL;
        return -1;
    }
    static const char letters[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    unsigned seed = (unsigned)getpid() * 2654435761u ^ (unsigned)rand();
    for (int attempt = 0; attempt < 100; attempt++) {
        unsigned v = seed + (unsigned)attempt * 7919u;
        for (int i = 0; i < 6; i++) {
            template[len - 6 + i] = letters[v % (sizeof letters - 1)];
            v /= (sizeof letters - 1);
        }
        int fd = open(template, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0)
            return fd;
        if (errno != EEXIST)
            return -1;
    }
    errno = EEXIST;
    return -1;
}

void *bsearch(const void *key, const void *base, size_t n, size_t size,
              int (*cmp)(const void *, const void *))
{
    const char *b = base;
    while (n > 0) {
        size_t mid = n / 2;
        const char *p = b + mid * size;
        int r = cmp(key, p);
        if (r == 0)
            return (void *)p;
        if (r > 0) {
            b = p + size;
            n -= mid + 1;
        } else {
            n = mid;
        }
    }
    return NULL;
}

/* Park-Miller minimal standard generator, state in [1, 2^31 - 2]. */
static unsigned long random_state = 1;

long random(void)
{
    random_state = (random_state * 48271UL) % 2147483647UL;
    return (long)random_state - 1;
}

void srandom(unsigned seed)
{
    random_state = seed % 2147483647UL;
    if (random_state == 0)
        random_state = 1;
}

static const char *progname = "";

const char *getprogname(void)
{
    return progname;
}

void setprogname(const char *name)
{
    const char *slash = strrchr(name, '/');
    progname = slash != NULL ? slash + 1 : name;
}

/* long is 64 bits on x86_64, so the long long conversions are the long
 * ones. */
long long strtoll(const char *s, char **end, int base)
{
    return strtol(s, end, base);
}

unsigned long long strtoull(const char *s, char **end, int base)
{
    return strtoul(s, end, base);
}

long double strtold(const char *s, char **end)
{
    return strtod(s, end);
}

intmax_t strtoimax(const char *s, char **end, int base)
{
    return strtol(s, end, base);
}

uintmax_t strtoumax(const char *s, char **end, int base)
{
    return strtoul(s, end, base);
}

intmax_t imaxabs(intmax_t v)
{
    return v < 0 ? -v : v;
}
