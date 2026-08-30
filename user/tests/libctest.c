/* M10 test: libc behaviour. Exits 0 on success. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <assert.h>
#include <unistd.h>

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

static int atexit_ran;
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
    CHECK(strspn("aabbc", "ab") == 4 && strcspn("aabbc", "c") == 4, "strspn");
    CHECK(memcmp("abc", "abd", 3) < 0 && memcmp("abc", "abc", 3) == 0, "memcmp");
    char mv[16] = "0123456789";
    memmove(mv + 2, mv, 8);
    CHECK(strcmp(mv, "0101234567") == 0, "memmove overlap: %s", mv);
    char *dup = strdup("dup");
    CHECK(dup && strcmp(dup, "dup") == 0, "strdup");
    free(dup);
    char toks[] = "a,b,,c";
    const char *save;
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

    CHECK(atexit(at_exit) == 0, "atexit");
    printf("libctest: %d failures\n", failures);
    return failures ? 1 : 0;
}
