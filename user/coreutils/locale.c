/* locale: print the locale of each category, the installed locales, or
 * the values of locale keywords (locale(1), docs/design/locale.md). */
#include <dirent.h>
#include <langinfo.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct { const char *name; int category; } categories[] = {
    { "LC_CTYPE", LC_CTYPE }, { "LC_NUMERIC", LC_NUMERIC }, { "LC_TIME", LC_TIME },
    { "LC_COLLATE", LC_COLLATE }, { "LC_MONETARY", LC_MONETARY }, { "LC_MESSAGES", LC_MESSAGES },
};

/* A keyword names one item, or a list of count items from item. */
static const struct { const char *name; nl_item item; int count; } keywords[] = {
    { "codeset", CODESET, 1 }, { "decimal_point", RADIXCHAR, 1 }, { "thousands_sep", THOUSEP, 1 },
    { "d_t_fmt", D_T_FMT, 1 }, { "d_fmt", D_FMT, 1 }, { "t_fmt", T_FMT, 1 }, { "t_fmt_ampm", T_FMT_AMPM, 1 },
    { "date_fmt", _DATE_FMT, 1 }, { "am_pm", AM_STR, 2 }, { "day", DAY_1, 7 }, { "abday", ABDAY_1, 7 },
    { "mon", MON_1, 12 }, { "abmon", ABMON_1, 12 }, { "alt_mon", ALTMON_1, 12 }, { "yesexpr", YESEXPR, 1 },
    { "noexpr", NOEXPR, 1 }, { "crncystr", CRNCYSTR, 1 },
};

static int cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int list_all(void)
{
    puts("C");
    puts("C.UTF-8");
    puts("POSIX");
    DIR *dir = opendir("/usr/share/i18n/locales");
    if (!dir)
        return 0;
    char *names[64];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(dir)) != NULL && n < 64)
        if (e->d_name[0] != '.')
            names[n++] = strdup(e->d_name);
    closedir(dir);
    qsort(names, (size_t)n, sizeof names[0], cmp);
    for (int i = 0; i < n; i++) {
        printf("%s.UTF-8\n", names[i]);
        free(names[i]);
    }
    return 0;
}

static void show_categories(void)
{
    const char *lang = getenv("LANG"), *all = getenv("LC_ALL");
    printf("LANG=%s\n", lang ? lang : "");
    for (size_t i = 0; i < sizeof categories / sizeof categories[0]; i++) {
        const char *set = getenv(categories[i].name);
        const char *name = setlocale(categories[i].category, NULL);
        /* A value that only LANG or LC_ALL implies is quoted, as POSIX
         * requires. */
        if (set && *set && !(all && *all))
            printf("%s=%s\n", categories[i].name, name);
        else
            printf("%s=\"%s\"\n", categories[i].name, name);
    }
    printf("LC_ALL=%s\n", all ? all : "");
}

static int show_keyword(const char *word, int with_name)
{
    for (size_t i = 0; i < sizeof keywords / sizeof keywords[0]; i++) {
        if (strcmp(word, keywords[i].name) != 0)
            continue;
        if (with_name)
            printf("%s=", word);
        printf("\"");
        for (int k = 0; k < keywords[i].count; k++)
            printf("%s%s", k ? ";" : "", nl_langinfo(keywords[i].item + k));
        printf("\"\n");
        return 0;
    }
    fprintf(stderr, "locale: unknown keyword %s\n", word);
    return 1;
}

int main(int argc, char **argv)
{
    if (!setlocale(LC_ALL, ""))
        fprintf(stderr, "locale: cannot set the locale of the environment; using C\n");
    int i = 1, with_name = 0;
    if (i < argc && strcmp(argv[i], "-a") == 0)
        return list_all();
    if (i < argc && strcmp(argv[i], "-k") == 0) {
        with_name = 1;
        i++;
    }
    if (i == argc) {
        show_categories();
        return 0;
    }
    int status = 0;
    for (; i < argc; i++)
        status |= show_keyword(argv[i], with_name);
    return status;
}
