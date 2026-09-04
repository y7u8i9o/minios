/* man: find and display plain-text manual pages from /usr/share/man. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <sys/wait.h>

static const char *sections[] = { "1", "2", "3", "4", "5", "7", "8", NULL };

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static int contains_case(const char *s, const char *needle)
{
    if (!*needle)
        return 1;
    for (; *s; s++) {
        size_t i = 0;
        while (needle[i] && s[i] && lower(s[i]) == lower(needle[i]))
            i++;
        if (!needle[i])
            return 1;
    }
    return 0;
}

static int page_path(char *path, size_t size, const char *section, const char *name)
{
    snprintf(path, size, "/usr/share/man/man%s/%s.%s", section, name, section);
    FILE *f = fopen(path, "r");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

static int find_page(char *path, size_t size, const char *section, const char *name)
{
    if (section)
        return page_path(path, size, section, name);
    for (int i = 0; sections[i]; i++)
        if (page_path(path, size, sections[i], name))
            return 1;
    return 0;
}

static int copy_page(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return 1;
    char buf[1024];
    while (fgets(buf, sizeof buf, f))
        fputs(buf, stdout);
    int r = ferror(f);
    fclose(f);
    return r;
}

static int show_page(const char *path)
{
    if (!isatty(1))
        return copy_page(path);
    const char *pager = getenv("MANPAGER");
    if (!pager || !*pager)
        pager = getenv("PAGER");
    if (!pager || !*pager)
        pager = "pager";
    char *av[] = { (char *)pager, (char *)path, NULL };
    pid_t pid = fork();
    if (pid < 0)
        return copy_page(path);
    if (pid == 0) {
        execvp(av[0], av);
        _exit(127);
    }
    int status;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
}

static void description(FILE *f, char *out, size_t size)
{
    char line[512];
    out[0] = '\0';
    int name_heading = 0;
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r'))
            line[--n] = '\0';
        if (strcmp(line, "NAME") == 0) {
            name_heading = 1;
            continue;
        }
        if (name_heading && line[0]) {
            strlcpy(out, line, size);
            return;
        }
    }
}

static int apropos(const char *word, int exact)
{
    int found = 0;
    for (int si = 0; sections[si]; si++) {
        char dirpath[64];
        snprintf(dirpath, sizeof dirpath, "/usr/share/man/man%s", sections[si]);
        DIR *d = opendir(dirpath);
        if (!d)
            continue;
        struct dirent *e;
        while ((e = readdir(d))) {
            size_t n = strlen(e->d_name), sn = strlen(sections[si]);
            if (n <= sn + 1 || e->d_name[n - sn - 1] != '.' ||
                strcmp(e->d_name + n - sn, sections[si]) != 0)
                continue;
            char name[256], path[512], desc[512];
            snprintf(name, sizeof name, "%.*s", (int)(n - sn - 1), e->d_name);
            if (exact && strcmp(name, word) != 0)
                continue;
            snprintf(path, sizeof path, "%s/%s", dirpath, e->d_name);
            FILE *f = fopen(path, "r");
            if (!f)
                continue;
            description(f, desc, sizeof desc);
            fclose(f);
            if (!exact && !contains_case(name, word) && !contains_case(desc, word))
                continue;
            const char *summary = strstr(desc, " - ");
            summary = summary ? summary + 3 : (desc[0] ? desc : "manual page");
            printf("%s (%s) - %s\n", name, sections[si], summary);
            found = 1;
        }
        closedir(d);
    }
    return found ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc == 3 && strcmp(argv[1], "-k") == 0)
        return apropos(argv[2], 0);
    if (argc == 3 && strcmp(argv[1], "-f") == 0)
        return apropos(argv[2], 1);
    int where = 0, i = 1;
    if (i < argc && strcmp(argv[i], "-w") == 0) {
        where = 1;
        i++;
    }
    const char *section = NULL;
    if (argc - i == 2) {
        section = argv[i++];
    } else if (argc - i != 1) {
        fprintf(stderr, "usage: man [-w] [section] name\n"
                        "       man -k keyword\n"
                        "       man -f name\n");
        return 2;
    }
    char path[512];
    if (!find_page(path, sizeof path, section, argv[i])) {
        fprintf(stderr, "man: no manual entry for %s%s%s\n",
                argv[i], section ? " in section " : "", section ? section : "");
        return 1;
    }
    if (where) {
        printf("%s\n", path);
        return 0;
    }
    return show_page(path);
}
