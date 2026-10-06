/* L1: named locales.  setlocale and the environment, localeconv, the
 * radix character and digit grouping of printf, strtod and scanf, the
 * names and formats of strftime, nl_langinfo, collation, locale objects
 * in a second thread, locale -a and Lua's os.setlocale. */
#include <errno.h>
#include <langinfo.h>
#include <locale.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <sys/wait.h>

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("localetest: FAIL " __VA_ARGS__); printf("\n"); } } while (0)

static int same(const char *got, const char *want, const char *what)
{
    if (got && strcmp(got, want) == 0)
        return 1;
    failures++;
    printf("localetest: FAIL %s: \"%s\", expected \"%s\"\n", what, got ? got : "(null)", want);
    return 0;
}

static void test_names(void)
{
    same(setlocale(LC_ALL, NULL), "C", "initial locale");
    same(setlocale(LC_ALL, "fr_FR.UTF-8"), "fr_FR.UTF-8", "full name");
    same(setlocale(LC_ALL, "es_ES"), "es_ES.UTF-8", "name without a codeset");
    same(setlocale(LC_ALL, "ru_RU.utf8"), "ru_RU.UTF-8", "codeset utf8");
    same(setlocale(LC_ALL, "ja"), "ja_JP.UTF-8", "language only");
    CHECK(setlocale(LC_ALL, "xx_YY") == NULL, "unknown locale accepted");
    CHECK(setlocale(LC_ALL, "fr_FR.ISO-8859-1") == NULL, "codeset other than UTF-8 accepted");
    same(setlocale(LC_ALL, NULL), "ja_JP.UTF-8", "failed calls leave the locale");
    same(setlocale(LC_ALL, "C.UTF-8"), "C.UTF-8", "C.UTF-8");
    same(setlocale(LC_ALL, "POSIX"), "C", "POSIX");

    setenv("LANG", "es_ES.UTF-8", 1);
    setenv("LC_TIME", "ru_RU.UTF-8", 1);
    unsetenv("LC_ALL");
    setlocale(LC_ALL, "");
    same(setlocale(LC_NUMERIC, NULL), "es_ES.UTF-8", "LANG gives LC_NUMERIC");
    same(setlocale(LC_TIME, NULL), "ru_RU.UTF-8", "LC_TIME overrides LANG");
    same(setlocale(LC_ALL, NULL),
         "LC_COLLATE=es_ES.UTF-8;LC_CTYPE=es_ES.UTF-8;LC_MONETARY=es_ES.UTF-8;LC_NUMERIC=es_ES.UTF-8;"
         "LC_TIME=ru_RU.UTF-8;LC_MESSAGES=es_ES.UTF-8", "composite name");
    char composite[300];
    strlcpy(composite, setlocale(LC_ALL, NULL), sizeof composite);
    setlocale(LC_ALL, "C");
    same(setlocale(LC_ALL, composite), composite, "a composite name is accepted");
    setenv("LC_ALL", "fr_FR.UTF-8", 1);
    setlocale(LC_ALL, "");
    same(setlocale(LC_TIME, NULL), "fr_FR.UTF-8", "LC_ALL overrides LC_TIME");
    unsetenv("LC_ALL");
    unsetenv("LC_TIME");
    unsetenv("LANG");
    setlocale(LC_ALL, "C");
}

static void test_numbers(void)
{
    static const struct { const char *locale, *point, *f, *grouped, *grouped_f; } cases[] = {
        { "C", ".", "1234.50", "1234567", "1234567.25" },
        { "en_US", ".", "1234.50", "1,234,567", "1,234,567.25" },
        { "fr_FR", ",", "1234,50", "1 234 567", "1 234 567,25" },
        { "es_ES", ",", "1234,50", "1.234.567", "1.234.567,25" },
        { "ru_RU", ",", "1234,50", "1 234 567", "1 234 567,25" },
        { "zh_CN", ".", "1234.50", "1,234,567", "1,234,567.25" },
        { "ja_JP", ".", "1234.50", "1,234,567", "1,234,567.25" },
    };
    char buf[64], what[64];
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        setlocale(LC_ALL, cases[i].locale);
        snprintf(what, sizeof what, "%s decimal point", cases[i].locale);
        same(localeconv()->decimal_point, cases[i].point, what);
        snprintf(what, sizeof what, "%s %%.2f", cases[i].locale);
        snprintf(buf, sizeof buf, "%.2f", 1234.5);
        same(buf, cases[i].f, what);
        snprintf(what, sizeof what, "%s %%'d", cases[i].locale);
        snprintf(buf, sizeof buf, "%'d", 1234567);
        same(buf, cases[i].grouped, what);
        snprintf(what, sizeof what, "%s %%'.2f", cases[i].locale);
        snprintf(buf, sizeof buf, "%'.2f", 1234567.25);
        same(buf, cases[i].grouped_f, what);
    }
    setlocale(LC_ALL, "fr_FR.UTF-8");
    char *end;
    double v = strtod("3,25x", &end);
    CHECK(v == 3.25 && *end == 'x', "strtod with a decimal comma: %g", v);
    v = strtod("3.25", &end);
    CHECK(v == 3.0 && *end == '.', "a full stop ends a number in fr_FR: %g", v);
    double s = 0;
    CHECK(sscanf("2,5", "%lf", &s) == 1 && s == 2.5, "sscanf with a decimal comma: %g", s);
    snprintf(buf, sizeof buf, "%g %e", 0.5, 1.5);
    same(buf, "0,5 1,500000e+00", "fr_FR %g %e");
    setlocale(LC_ALL, "C");
    CHECK(strtod("3.25", NULL) == 3.25, "strtod in C");
    snprintf(buf, sizeof buf, "%'d", 1234567);
    same(buf, "1234567", "C has no grouping");
}

static void test_time(void)
{
    /* Monday 5 January 2026, 14:07:09. */
    struct tm tm = { .tm_year = 126, .tm_mon = 0, .tm_mday = 5, .tm_hour = 14, .tm_min = 7, .tm_sec = 9,
                     .tm_wday = 1, .tm_yday = 4 };
    static const struct { const char *locale, *format, *want; } cases[] = {
        { "C", "%c", "Mon Jan  5 14:07:09 2026" },
        { "C", "%x %X %r", "01/05/26 14:07:09 02:07:09 PM" },
        { "en_US", "%x %r", "01/05/2026 02:07:09 PM" },
        { "fr_FR", "%A %d %B %Y", "lundi 05 janvier 2026" },
        { "fr_FR", "%c", "lun. 05 janv. 2026 14:07:09 UTC" },
        { "fr_FR", "%x %X", "05/01/2026 14:07:09" },
        { "es_ES", "%A %e de %B", "lunes  5 de enero" },
        { "ru_RU", "%d %B %Y", "05 января 2026" },
        { "ru_RU", "%OB, %a", "январь, Пн" },
        { "ru_RU", "%x", "05.01.2026" },
        { "zh_CN", "%x %A", "2026年01月05日 星期一" },
        { "zh_CN", "%p %X", "下午 14时07分09秒" },
        { "ja_JP", "%x %A %p", "2026年01月05日 月曜日 午後" },
    };
    char buf[128], what[64];
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        setlocale(LC_ALL, cases[i].locale);
        strftime(buf, sizeof buf, cases[i].format, &tm);
        snprintf(what, sizeof what, "%s strftime %s", cases[i].locale, cases[i].format);
        same(buf, cases[i].want, what);
    }
    setlocale(LC_ALL, "C");
    /* Friday 1 January 2021 is in week 53 of 2020. */
    struct tm d = { .tm_year = 121, .tm_mon = 0, .tm_mday = 1, .tm_hour = 9, .tm_wday = 5, .tm_yday = 0 };
    strftime(buf, sizeof buf, "%G %g %V %U %W %k|%l|%P", &d);
    same(buf, "2020 20 53 00 00  9| 9|am", "ISO week and the new conversions");
    /* Thursday 31 December 2026 is in week 53 of 2026, Monday 29 December
     * 2025 in week 1 of 2026. */
    struct tm e = { .tm_year = 126, .tm_mon = 11, .tm_mday = 31, .tm_wday = 4, .tm_yday = 364 };
    struct tm f = { .tm_year = 125, .tm_mon = 11, .tm_mday = 29, .tm_wday = 1, .tm_yday = 362 };
    char b2[32];
    strftime(buf, sizeof buf, "%G-%V", &e);
    strftime(b2, sizeof b2, "%G-%V", &f);
    CHECK(strcmp(buf, "2026-53") == 0 && strcmp(b2, "2026-01") == 0, "ISO weeks at year ends: %s %s", buf, b2);
    struct tm epoch = { .tm_year = 70, .tm_mday = 2, .tm_wday = 5, .tm_yday = 1 };
    strftime(buf, sizeof buf, "%s", &epoch);
    same(buf, "86400", "%s");
}

static void test_langinfo(void)
{
    setlocale(LC_ALL, "ru_RU.UTF-8");
    same(nl_langinfo(CODESET), "UTF-8", "CODESET");
    same(nl_langinfo(MON_1), "января", "MON_1");
    same(nl_langinfo(ALTMON_1), "январь", "ALTMON_1");
    same(nl_langinfo(YESEXPR), "^[+1yYдД]", "YESEXPR");
    same(nl_langinfo(CRNCYSTR), "+₽", "CRNCYSTR");
    same(nl_langinfo(RADIXCHAR), ",", "RADIXCHAR");
    same(nl_langinfo(_NL_FIRST_WEEKDAY), "1", "the week of ru_RU begins on Monday");
    setlocale(LC_ALL, "ja_JP.UTF-8");
    same(nl_langinfo(DAY_1), "日曜日", "DAY_1");
    same(nl_langinfo(_NL_FIRST_WEEKDAY), "0", "the week of ja_JP begins on Sunday");
    same(nl_langinfo(CRNCYSTR), "-￥", "CRNCYSTR of ja_JP");
    CHECK(localeconv()->frac_digits == 0, "frac_digits of ja_JP");
    setlocale(LC_ALL, "C");
    same(nl_langinfo(ABMON_12), "Dec", "ABMON_12 in C");
    same(nl_langinfo(_NL_FIRST_WEEKDAY), "0", "the week of C begins on Sunday");
    CHECK(localeconv()->frac_digits == __SCHAR_MAX__, "frac_digits of C");
}

static int cmp_coll(const void *a, const void *b)
{
    return strcoll(*(const char *const *)a, *(const char *const *)b);
}

static void check_order(const char *locale, const char **words, int n, const char *want)
{
    setlocale(LC_COLLATE, locale);
    qsort(words, (size_t)n, sizeof words[0], cmp_coll);
    char got[256] = "";
    for (int i = 0; i < n; i++) {
        if (i)
            strlcat(got, " ", sizeof got);
        strlcat(got, words[i], sizeof got);
    }
    char what[64];
    snprintf(what, sizeof what, "%s collation", locale);
    same(got, want, what);
    /* strxfrm orders like strcoll. */
    for (int i = 0; i + 1 < n; i++) {
        char ka[256], kb[256];
        size_t la = strxfrm(ka, words[i], sizeof ka), lb = strxfrm(kb, words[i + 1], sizeof kb);
        CHECK(la < sizeof ka && lb < sizeof kb && strcmp(ka, kb) <= 0, "%s strxfrm orders %s before %s", locale,
              words[i], words[i + 1]);
    }
}

static void test_collation(void)
{
    const char *en[] = { "banana", "Zebra", "apple", "éclair", "Apple", "10", " x", "2" };
    check_order("en_US", en, 8, " x 10 2 apple Apple banana éclair Zebra");
    const char *fr[] = { "côté", "coté", "côte", "cote" };
    check_order("fr_FR", fr, 4, "cote coté côte côté");
    const char *es[] = { "oa", "ñu", "Nb", "nz" };
    check_order("es_ES", es, 4, "Nb nz ñu oa");
    const char *ru[] = { "ель", "ёж", "еж", "Ёлка", "яма", "Арбуз" };
    check_order("ru_RU", ru, 6, "Арбуз еж ёж Ёлка ель яма");
    const char *c[] = { "b", "a", "B", "é" };
    check_order("C", c, 4, "B a b é");
    setlocale(LC_COLLATE, "en_US.UTF-8");
    CHECK(wcscoll(L"apple", L"Banana") < 0 && wcscoll(L"é", L"f") < 0, "wcscoll");
    setlocale(LC_ALL, "C");
}

static void *thread_main(void *arg)
{
    locale_t fr = arg;
    locale_t old = uselocale(fr);
    char buf[32];
    snprintf(buf, sizeof buf, "%.1f", 2.5);
    int ok = old == LC_GLOBAL_LOCALE && strcmp(buf, "2,5") == 0 && strcmp(localeconv()->decimal_point, ",") == 0;
    uselocale(LC_GLOBAL_LOCALE);
    return ok ? (void *)1 : NULL;
}

static void test_objects(void)
{
    errno = 0;
    CHECK(newlocale(LC_ALL_MASK, "de_DE.UTF-8", NULL) == NULL && errno == ENOENT, "newlocale of a missing locale");
    locale_t fr = newlocale(LC_NUMERIC_MASK | LC_TIME_MASK, "fr_FR.UTF-8", NULL);
    CHECK(fr != NULL, "newlocale fr_FR");
    if (!fr)
        return;
    struct tm tm = { .tm_year = 126, .tm_mon = 6, .tm_mday = 14, .tm_wday = 2, .tm_yday = 194 };
    char buf[64];
    strftime_l(buf, sizeof buf, "%A %d %B", &tm, fr);
    same(buf, "mardi 14 juillet", "strftime_l");
    same(nl_langinfo_l(RADIXCHAR, fr), ",", "nl_langinfo_l");
    CHECK(strtod_l("1,5", NULL, fr) == 1.5, "strtod_l");
    same(nl_langinfo_l(YESEXPR, fr), "^[yY]", "categories outside the mask remain C");
    pthread_t t;
    void *result = NULL;
    CHECK(pthread_create(&t, NULL, thread_main, fr) == 0 && pthread_join(t, &result) == 0 && result,
          "uselocale in a second thread");
    snprintf(buf, sizeof buf, "%.1f", 2.5);
    same(buf, "2.5", "the main thread uses the global locale");
    locale_t copy = duplocale(fr);
    fr = newlocale(LC_COLLATE_MASK, "es_ES.UTF-8", fr);
    CHECK(fr && strcoll_l("ñ", "o", fr) < 0 && strcoll_l("ñ", "o", copy) > 0, "newlocale modifies its base");
    freelocale(copy);
    freelocale(fr);
}

/* run executes a program and returns its standard output. */
static int run(char *const argv[], char *out, size_t size)
{
    int p[2];
    if (pipe(p) < 0)
        return -1;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(p[1], 1);
        close(p[0]);
        close(p[1]);
        execv(argv[0], argv);
        _exit(127);
    }
    close(p[1]);
    size_t n = 0;
    ssize_t r;
    while (n < size - 1 && (r = read(p[0], out + n, size - 1 - n)) > 0)
        n += (size_t)r;
    out[n] = '\0';
    close(p[0]);
    int status;
    waitpid(pid, &status, 0);
    return status;
}

static void test_programs(void)
{
    char out[1024];
    run((char *const[]){ "/bin/locale", "-a", NULL }, out, sizeof out);
    CHECK(strstr(out, "C.UTF-8\n") && strstr(out, "en_US.UTF-8\n") && strstr(out, "fr_FR.UTF-8\n") &&
          strstr(out, "es_ES.UTF-8\n") && strstr(out, "ru_RU.UTF-8\n") && strstr(out, "zh_CN.UTF-8\n") &&
          strstr(out, "ja_JP.UTF-8\n"), "locale -a lists the locales: %s", out);
    setenv("LANG", "ru_RU.UTF-8", 1);
    run((char *const[]){ "/bin/locale", "-k", "decimal_point", "alt_mon", NULL }, out, sizeof out);
    CHECK(strstr(out, "decimal_point=\",\"\n") == out && strstr(out, "\nalt_mon=\"январь;февраль;"),
          "locale -k: %s", out);
    unsetenv("LANG");
    run((char *const[]){ "/bin/lua", "-e", "print(os.setlocale('fr_FR.UTF-8'), os.date('!%A %B', 0), 1.5)", NULL },
        out, sizeof out);
    same(out, "fr_FR.UTF-8\tjeudi janvier\t1,5\n", "Lua os.setlocale and os.date");
}

int main(void)
{
    test_names();
    test_numbers();
    test_time();
    test_langinfo();
    test_collation();
    test_objects();
    test_programs();
    printf("localetest: %d failures\n", failures);
    return failures != 0;
}
