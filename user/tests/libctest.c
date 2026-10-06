/* M10 test: libc behaviour. Exits 0 on success. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>
#include <minios/proctab.h>
#include <sys/random.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void fmt(const char *expect, const char *f, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, f);
    int n = vsnprintf(buf, sizeof buf, f, ap);
    va_end(ap);
    CHECK(strcmp(buf, expect) == 0 && n == (int)strlen(expect), "format \"%s\" -> \"%s\" (%d), expected \"%s\"", f, buf, n, expect);
}

static int cmp_int(const void *a, const void *b)
{
    return *(const int *)a - *(const int *)b;
}

/* memcpy, memmove and memset at every relative alignment (G2 of
 * docs/plan/compositor-performance.md). Each call is compared with a byte
 * loop, and the 16 bytes on each side of the destination must remain. */
#define MEM_GUARD 16
#define MEM_AREA (MEM_GUARD + 32 + 65537 + MEM_GUARD)
static unsigned char mem_src[MEM_AREA], mem_dst[MEM_AREA], mem_ref[MEM_AREA];

static const size_t mem_lengths[] = { 0,   1,   2,   3,   4,   5,   7,   8,   9,   12,  15,  16,  17,  24,
                                      31,  32,  33,  47,  48,  49,  63,  64,  65,  79,  80,  95,  96,  127,
                                      128, 129, 191, 192, 193, 255, 256, 257, 299, 300, 65537 };
#define MEM_NLENGTHS (sizeof mem_lengths / sizeof mem_lengths[0])

static void mem_fill(unsigned char *p, size_t n, unsigned seed)
{
    for (size_t i = 0; i < n; i++)
        p[i] = (unsigned char)(i * 7 + seed * 13 + (i >> 8));
}

/* The reference copy is a loop over volatile bytes, which GCC cannot turn
 * into a call of the memcpy under test. */
static void mem_copy_bytes(unsigned char *to, const unsigned char *from, size_t n)
{
    volatile unsigned char *t = to;
    for (size_t i = 0; i < n; i++)
        t[i] = from[i];
}

static int mem_same(const unsigned char *a, const unsigned char *b, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static void check_memcpy(void)
{
    mem_fill(mem_src, MEM_AREA, 1);
    for (size_t so = 0; so < 32; so++)
        for (size_t dof = 0; dof < 32; dof++)
            for (size_t k = 0; k < MEM_NLENGTHS; k++) {
                size_t n = mem_lengths[k];
                /* The longest length runs at four offset pairs only. */
                if (n > 300 && (so % 16 || dof % 15))
                    continue;
                size_t span = MEM_GUARD + dof + n + MEM_GUARD;
                mem_fill(mem_dst, span, 2);
                mem_copy_bytes(mem_ref, mem_dst, span);
                mem_copy_bytes(mem_ref + MEM_GUARD + dof, mem_src + so, n);
                void *r = memcpy(mem_dst + MEM_GUARD + dof, mem_src + so, n);
                if (r != mem_dst + MEM_GUARD + dof || !mem_same(mem_dst, mem_ref, span)) {
                    CHECK(0, "memcpy source offset %zu, destination offset %zu, length %zu", so, dof, n);
                    return;
                }
            }
}

static void check_memmove(void)
{
    for (size_t a = 0; a < 33; a++)
        for (size_t b = 0; b < 33; b++)
            for (size_t k = 0; k < MEM_NLENGTHS; k++) {
                size_t n = mem_lengths[k];
                if (n > 300)
                    continue;
                size_t span = MEM_GUARD + 33 + n + MEM_GUARD;
                mem_fill(mem_dst, span, 3);
                mem_copy_bytes(mem_ref, mem_dst, span);
                mem_copy_bytes(mem_src, mem_dst + MEM_GUARD + b, n);
                mem_copy_bytes(mem_ref + MEM_GUARD + a, mem_src, n);
                void *r = memmove(mem_dst + MEM_GUARD + a, mem_dst + MEM_GUARD + b, n);
                if (r != mem_dst + MEM_GUARD + a || !mem_same(mem_dst, mem_ref, span)) {
                    CHECK(0, "memmove destination %zu, source %zu, length %zu", a, b, n);
                    return;
                }
            }
}

static void check_memset(void)
{
    static const int values[] = { 0, 0x5a, 0xff, 0x1a5 };
    for (size_t v = 0; v < 4; v++)
        for (size_t dof = 0; dof < 32; dof++)
            for (size_t k = 0; k < MEM_NLENGTHS; k++) {
                size_t n = mem_lengths[k];
                if (n > 300 && dof % 16)
                    continue;
                size_t span = MEM_GUARD + dof + n + MEM_GUARD;
                mem_fill(mem_dst, span, 4);
                mem_copy_bytes(mem_ref, mem_dst, span);
                for (size_t i = 0; i < n; i++)
                    mem_ref[MEM_GUARD + dof + i] = (unsigned char)values[v];
                void *r = memset(mem_dst + MEM_GUARD + dof, values[v], n);
                if (r != mem_dst + MEM_GUARD + dof || !mem_same(mem_dst, mem_ref, span)) {
                    CHECK(0, "memset value 0x%x, offset %zu, length %zu", values[v], dof, n);
                    return;
                }
            }
}

/* Megabytes per second of 32 copies or fills of 1 MiB. */
static unsigned mem_rate(int fill, size_t dst_offset)
{
    size_t n = (size_t)1 << 20;
    unsigned char *from = malloc(n + 64), *to = malloc(n + 64);
    if (!from || !to) {
        free(from);
        free(to);
        return 0;
    }
    memset(from, 1, n + 64);
    memset(to, 2, n + 64);
    long t0 = uptime_us();
    for (int i = 0; i < 32; i++) {
        if (fill)
            memset(to + dst_offset, i, n);
        else
            memcpy(to + dst_offset, from, n);
    }
    long us = uptime_us() - t0;
    free(from);
    free(to);
    return us > 0 ? (unsigned)(32ull * 1000000 / (unsigned long long)us) : 0;
}

static void check_memory_functions(void)
{
    check_memcpy();
    check_memmove();
    check_memset();
    printf("libctest: memcpy %u MB/s aligned, %u MB/s at offset 4, %u MB/s at offset 1, memset %u MB/s\n",
           mem_rate(0, 0), mem_rate(0, 4), mem_rate(0, 1), mem_rate(1, 0));
}

static int atexit_ran;
/* The process table contains this process with its parent, uid and
 * name, and the kernel as pid 0. */
static void check_proc_table(void)
{
    struct proc_entry rows[64], self;
    int n = proc_table_read(rows, 64);
    int kernel = 0;
    for (int i = 0; i < n; i++)
        kernel |= rows[i].pid == 0 && strcmp(rows[i].name, "kernel") == 0;
    CHECK(n >= 2 && kernel, "proc_table_read: %d rows, kernel row %d", n, kernel);
    CHECK(proc_table_find(getpid(), &self) == 0 && self.ppid == getppid() && self.uid == geteuid() &&
              strcmp(self.name, "libctest") == 0 && strcmp(self.state, "running") == 0 && self.rss_kib > 0,
          "proc_table_find: ppid %d uid %u name %s state %s rss %lu", (int)self.ppid, self.uid, self.name,
          self.state, self.rss_kib);
    CHECK(proc_table_find(99999, &self) == -ESRCH, "proc_table_find of a missing pid");
    CHECK(proc_table_read(rows, 1) == 1, "proc_table_read limits the rows");
}

/* The kernel generator gives different bytes on each call. */
static void check_random(void)
{
    unsigned char a[64], b[64];
    CHECK(getrandom(a, sizeof a, 0) == (ssize_t)sizeof a && getentropy(b, sizeof b) == 0 && memcmp(a, b, 64) != 0,
          "getrandom and getentropy give different bytes");
    unsigned char big[300];
    CHECK(getrandom(big, sizeof big, 0) == 256, "getrandom gives at most 256 bytes per call");
    CHECK(getentropy(big, sizeof big) < 0 && errno == EIO, "getentropy refuses more than 256 bytes");
    CHECK(getrandom(a, 8, 0x80) < 0 && errno == EINVAL, "getrandom refuses unknown flags");
}

static void at_exit(void)
{
    atexit_ran = 1;
    printf("libctest: atexit handler ran\n");
}

int main(int argc, char **argv)
{
    /* printf family */
    fmt("42", "%d", 42);
    fmt("-42", "%d", -42);
    fmt("  42", "%4d", 42);
    fmt("0042", "%04d", 42);
    fmt("42  |", "%-4d|", 42);
    fmt("+42", "%+d", 42);
    fmt("ff FF 0xff 377", "%x %X %#x %o", 255, 255, 255, 255);
    fmt("18446744073709551615", "%lu", ~0UL);
    fmt("-9223372036854775808", "%ld", (long)(-9223372036854775807L - 1));
    fmt("123456789012", "%lld", 123456789012LL);
    fmt("0x1000", "%p", (void *)0x1000);
    fmt("abc", "%s", "abc");
    fmt("ab", "%.2s", "abcdef");
    fmt("  abc", "%5s", "abc");
    fmt("abc  |", "%-5s|", "abc");
    fmt("x", "%c", 'x');
    fmt("%", "%%");
    fmt("123", "%zu", (size_t)123);
    fmt("(null)", "%s", (char *)NULL);
    fmt("007", "%.3d", 7);
    fmt("", "%.0d", 0);
    fmt("   7", "%*d", 4, 7);
    fmt("-1 255", "%hhd %hhu", 255, 255);
    char small[4];
    int n = snprintf(small, sizeof small, "%d", 123456);
    CHECK(n == 6 && strcmp(small, "123") == 0, "snprintf truncation: %d \"%s\"", n, small);

    /* string.h */
    char buf[64];
    strcpy(buf, "hello");
    strcat(buf, " world");
    CHECK(strcmp(buf, "hello world") == 0, "strcat");
    CHECK(strlen(buf) == 11, "strlen");
    CHECK(strncmp(buf, "hello", 5) == 0 && strncmp(buf, "help", 4) != 0, "strncmp");
    CHECK(strchr(buf, 'w') == buf + 6 && strrchr(buf, 'l') == buf + 9 && strchr(buf, 'z') == NULL, "strchr");
    CHECK(strstr(buf, "wor") == buf + 6 && strstr(buf, "xyz") == NULL, "strstr");
    {
        static const char bin[] = { 'a', 0, 'b', 0, 'b', 'c' };
        CHECK(memmem(bin, sizeof bin, "\0bc", 3) == bin + 3 && memmem(bin, sizeof bin, "bcd", 3) == NULL &&
                  memmem(bin, 2, "a", 0) == bin && memmem(bin, 2, "b", 1) == NULL,
              "memmem");
    }
    CHECK(strspn("aabbc", "ab") == 4 && strcspn("aabbc", "c") == 4, "strspn");
    CHECK(memcmp("abc", "abd", 3) < 0 && memcmp("abc", "abc", 3) == 0, "memcmp");
    char mv[16] = "0123456789";
    memmove(mv + 2, mv, 8);
    CHECK(strcmp(mv, "0101234567") == 0, "memmove overlap: %s", mv);
    char *dup = strdup("dup");
    CHECK(dup && strcmp(dup, "dup") == 0, "strdup");
    free(dup);
    char toks[] = "a,b,,c";
    char *save;
    char *t = strtok_r(toks, ",", &save);
    CHECK(t && strcmp(t, "a") == 0, "strtok 1");
    t = strtok_r(NULL, ",", &save);
    CHECK(t && strcmp(t, "b") == 0, "strtok 2");
    t = strtok_r(NULL, ",", &save);
    CHECK(t && strcmp(t, "c") == 0, "strtok 3");
    CHECK(strtok_r(NULL, ",", &save) == NULL, "strtok end");
    CHECK(strcmp(strerror(ENOENT), "No such file or directory") == 0, "strerror");

    /* ctype, stdlib conversions */
    CHECK(isdigit('5') && !isdigit('a') && isalpha('a') && isspace('\n') && toupper('a') == 'A', "ctype");
    CHECK(atoi("  -123abc") == -123 && atol("99999999999") == 99999999999L, "atoi");
    char *end;
    CHECK(strtol("0x1fZ", &end, 0) == 0x1f && *end == 'Z', "strtol hex");
    CHECK(strtol("0777", NULL, 0) == 0777 && strtol("-77", NULL, 10) == -77, "strtol octal");
    CHECK(strtoul("4294967296", NULL, 10) == 4294967296UL, "strtoul");
    errno = 0;
    CHECK(strtoul("99999999999999999999", NULL, 10) == ~0UL && errno == ERANGE, "strtoul overflow");
    CHECK(abs(-3) == 3 && labs(-4L) == 4, "abs");

    /* malloc */
    char *blocks[100];
    for (int i = 0; i < 100; i++) {
        blocks[i] = malloc((size_t)(i * 37 % 500 + 1));
        CHECK(blocks[i] != NULL, "malloc %d", i);
        memset(blocks[i], i, (size_t)(i * 37 % 500 + 1));
    }
    for (int i = 0; i < 100; i += 2)
        free(blocks[i]);
    for (int i = 1; i < 100; i += 2) {
        int ok = 1;
        for (size_t j = 0; j < (size_t)(i * 37 % 500 + 1); j++)
            if ((unsigned char)blocks[i][j] != (unsigned char)i) ok = 0;
        CHECK(ok, "malloc block %d corrupted", i);
        free(blocks[i]);
    }
    int *arr = calloc(10, sizeof(int));
    CHECK(arr && arr[9] == 0, "calloc");
    arr = realloc(arr, 1000 * sizeof(int));
    CHECK(arr != NULL, "realloc");
    arr[999] = 5;
    free(arr);
    void *big = malloc(2 * 1024 * 1024);
    CHECK(big != NULL, "large malloc");
    free(big);

    /* qsort, environment */
    int v[] = { 5, 3, 9, 1, 7 };
    qsort(v, 5, sizeof v[0], cmp_int);
    CHECK(v[0] == 1 && v[4] == 9 && v[2] == 5, "qsort");
    CHECK(getenv("PATH") && strcmp(getenv("PATH"), "/bin") == 0, "getenv PATH");
    CHECK(setenv("FOO", "bar", 1) == 0 && strcmp(getenv("FOO"), "bar") == 0, "setenv");
    CHECK(getenv("NOPE") == NULL, "getenv missing");

    /* stdio streams */
    CHECK(fputs("libctest: fputs to stdout\n", stdout) == 0, "fputs");
    fprintf(stderr, "libctest: fprintf to stderr\n");
    CHECK(fwrite("xyz\n", 1, 4, stdout) == 4, "fwrite");
    CHECK(puts("libctest: puts") == 0 && putchar('!') == '!' && putchar('\n') == '\n', "puts");
    CHECK(fflush(stdout) == 0, "fflush");
    CHECK(write(9, "x", 1) < 0 && errno == EBADF, "write bad fd");
    FILE *f = fopen("/bin/init", "r");
    CHECK(f != NULL && fgetc(f) == 0x7f && fclose(f) == 0, "fopen ELF header");
    CHECK(fopen("/nosuch", "r") == NULL && errno == ENOENT, "fopen missing");
    assert(1 + 1 == 2);

    check_memory_functions();
    check_proc_table();
    check_random();
    CHECK(atexit(at_exit) == 0, "atexit");
    printf("libctest: %d failures\n", failures);
    return failures ? 1 : 0;
}
